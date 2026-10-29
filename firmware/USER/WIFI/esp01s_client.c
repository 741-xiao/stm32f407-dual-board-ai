#include "esp01s.h"
#include "usart.h"
#include "uart_debug.h"
#include <string.h>

/*==============================================================================
 *  ESP-01 / AT 指令驱动  ——  非阻塞版
 *
 *  和旧版的区别只有一个，但就是它救了整个工程：
 *      旧版：ESP_WifiConnect() 一口气跑完（AT+RST -> CWJAP 等 20 秒 -> ...），
 *            在它返回之前，主循环一步都动不了 —— 按键、采集、LCD 全冻住。
 *      新版：把上面这一长串拆成一个个小状态，主循环每次调 Wifi_Tick()
 *            只推进一步就返回。联网那 20 秒里，采集照跑、按键照灵。
 *
 *  用法（在 App_Loop 里）：
 *      Wifi_Tick();                      // 每次主循环调一次，推进一步
 *      if (Wifi_IsReady()) Wifi_Push(buf, len);   // 就绪了就往上传一帧
 *
 *  ★ ESP-01S 挂在 **UART4（PA0=TX / PA1=RX）**；USART6（PC6/PC7）不再用于 ESP。
 *    （原先走 USART6，实测 PA0/PA1 更稳，已改；两个串口都要 115200。）
 *
 *  接收：UART4 开 1 字节中断，中断里把字节塞进环形缓冲；主循环轮询缓冲
 *        找期望关键字（OK / > / SEND OK）。不依赖 HAL_UART_Receive 的超时精度，
 *        连 CWJAP 那种好几秒才回来的长回显也不会丢。
 *
 *  注：源码里不写反斜杠转义（换行用 13U/10U 常量），避免编辑/工具链折腾编码。
 *============================================================================*/

#define ESP_RX_SIZE        512U
#define ESP_TX_SIZE        80U

/* ★ linktest v6：接收行缓冲放大到 1024。
   板1 双链路 + 回显，一次 Tick 里 esp_line 要同时容纳：
     AT 命令回显 + 数据帧回显 + 对端 +IPD(...) —— 512 容易在收集途中填满，
   ESP_Collect 一见 esp_len 到顶就停止搬环形缓冲 → 真正的 +IPD 被卡在环形里丢。
   放大到 1024，配合 ESP_StripEcho（剔除回显）后，容量绰绰有余。*/
#define ESP_LINE_SIZE      1024U

#define ESP_TMO_PWR_OFF    300U     /* 普通重连：模块断电持续时间 */
#define ESP_TMO_PWR_OFF_DEEP 5000U  /* 连续失败：断电更久，尽量让它彻底凉下来 */
#define ESP_TMO_PWR_ON     2000U    /* 等模块打完 74880 开机日志 */
#define ESP_TMO_RETRY      3000U    /* 掉线后歇多久再重连 */
#define ESP_RETRY_DEEP     3U       /* 连续失败几次后改用"深度断电" */
#define ESP_TMO_AT         2000U
#define ESP_TMO_JOIN       20000U
#define ESP_TMO_CONN       4000U
#define ESP_TMO_SEND       5000U
#define ESP_TMO_PROMPT     3000U
/* 两次发送之间的最小间隔。
   ★ 100ms 太紧：实测会在 100 <-> 300 之间来回震荡（发成功就缩回 100 ->
     下一发又撞 "busy s..." -> 退避 300 -> …），撞久了模块报 "link is not valid"
     直接把 TCP 断掉，那一次就丢 3~10 秒数据。
   1Hz 采样下每条数据要发 2 条链路(PC+板2)，250ms 间隔完全来得及，且不再撞忙。 */
#define ESP_TMO_GAP        250U

#define ESP_AT_RETRY_MAX   3U       /* 第一条 AT 最多原地重试几次 */

/* ---- CH_PD(EN) 在启明欣欣这块板上接的是 PF14 ---- */
#define ESP_EN_PORT        GPIOF
#define ESP_EN_PIN         GPIO_PIN_14
#define ESP_EN_ON          GPIO_PIN_SET      /* 拉高 = 模块工作 */
#define ESP_EN_OFF         GPIO_PIN_RESET    /* 拉低 = 模块断电 */

/* 把宏里的数字拼进 AT 指令：ESP_XSTR(8080) -> "8080" */
#define ESP_STR(x)     #x
#define ESP_XSTR(x)    ESP_STR(x)

/*------------------------------------------------------------------------------
 *  联网流程表：一条 AT 指令 = 一格。走完整个表就说明连上上位机了。
 *  要改流程、改超时，只改这张表，下面 Wifi_Tick() 不用动。
 *----------------------------------------------------------------------------*/
static const char *const at_cmd[] =
{
    "AT",                                                                 /* 0 模块在不在 */
    "ATE0",                                                               /* 1 关回显，日志清爽 */
    "AT+CWMODE=1",                                                        /* 2 STA 模式 */
    "AT+CWJAP=\"" ESP_WIFI_SSID "\",\"" ESP_WIFI_PASS "\"",               /* 3 连路由器 */
    "AT+CIPSTA=\"" ESP_STATIC_IP "\"",                                    /* 4 钉死本机 IP（绕开 DHCP 超时）*/
    "AT+CIPMUX=1",                                                        /* 5 多连接（实验六只用到 link0 = PC）*/
    "AT+CIPSTART=0,\"TCP\",\"" ESP_TCP_IP "\"," ESP_XSTR(ESP_TCP_PORT),   /* 6 link0 -> PC */
};

static const u16 at_tmo[] =
{
    ESP_TMO_AT, ESP_TMO_AT, ESP_TMO_AT, ESP_TMO_JOIN,
    ESP_TMO_AT, ESP_TMO_AT, ESP_TMO_CONN,
};

/* 哪几步允许失败。
   ★ 实验六：只有 link0(PC) 这条 TCP，且设成**可选步** ——
   开机时 PC 上位机通常还没跑，CIPSTART 会被拒（回 ERROR）；
   设可选后先跳过，等 PC 起来后按需限流补连（Wifi_TryLink(0)）。 */
static const u8 at_opt[] =
{
    0U, 0U, 0U, 0U, 0U, 0U, 1U,
};

#define AT_COUNT   (sizeof(at_cmd) / sizeof(at_cmd[0]))

/* at_cmd[] 里的关键下标（改动 at_cmd[] 顺序时必须同步维护）。 */
#define AT_STEP_CIPMUX   5U
#define AT_STEP_CIPSTART0 6U   /* link0 -> PC（可选，可后补）*/
/* ★ 实验六：已删除"link1 -> 板2"这一步（板间改走 CAN）。
   把下标设成 0xFF（**永不匹配**），相关 else-if 分支自然成为死代码，
   不必去改动那份调得很细的状态机。 */
#define AT_STEP_CIPSTART1 0xFFU

/* ★★ 本工程到底有没有 link1（对板那条 TCP）。
   ★ 存在意义：link_ok[1] 只有在"link1 的 CIPSTART 真跑过"时才可能变 1。
     实验六把 CIPSTART1 设成 0xFF 之后，link_ok[1] **恒为 0**，
     于是所有"link1 没连上就补连"的逻辑都成了**永远为真**的分支 ——
     实测事故：READY 里的周期补连每拍都命中 link_ok[1]==0、调用 Wifi_TryLink(1)
     又被守卫直接 return 0，调用处把它当成"已发起补连"→ `break`
     → **排在后面的发送闸门（取 TXQ + AT+CIPSEND）永远执行不到**。
     现象：PC 端 TCP 明明已连上，板端状态机 READY、link_ok[0]=1、队列也在排队，
     线上却一个字节都收不到（连 21 字节的 H 握手帧都没有）。
   ⇒ 凡是"按 link1 状态做决定"的地方，都必须用这个宏围起来。 */
#define WIFI_HAS_LINK1   (((AT_STEP_CIPSTART1) != 0xFFU) ? 1U : 0U)

static const u8 esp_crlf[3] = {13U, 10U, 0U};

/* ---- 接收环形缓冲（中断写、主循环读）---- */
static u8  esp_ring[ESP_RX_SIZE];
static volatile u16 esp_head;
static volatile u16 esp_tail;

static u8  esp_line[ESP_LINE_SIZE];   /* 从环形缓冲搬出来、补 0，便于查找子串 */
static u16 esp_len;
static u8  esp_rx_byte;

/*---- 对外状态 ----*/
u8 wifi_state = WIFI_ST_OFF;
u8 wifi_err   = 0U;

/*---- 状态机内部 ----*/
static u32 step_t0;         /* 当前子步骤的起点时刻 */
static u32 step_tmo;        /* 当前子步骤的超时（ms）*/
static u8  step_idx;        /* 走到流程表第几条 */
static u8  uart_ready;      /* UART4 是否已经初始化过 */
static u8  at_try;          /* 第一条 AT 撞上模块 "busy p..." 时原地重试了几次 */
static u8  retry_count;     /* 连续重连失败了几轮（连上就清零）*/
static u8  tx_fail;         /* 连续发送失败次数：单次失败不重连，>=3 才重来 */
static u32 tx_done_t0;      /* 上一帧发送结束的时刻（用来留发送间隔）*/
static u32 tx_gap_ms = ESP_TMO_GAP;     /* 当前发送间隔：模块一忙就自动拉长（退避）*/
static u8  busy_n;                      /* 连续收到几次 "busy"：退避按它递增 */
static u32 ready_t0;        /* 刚进入 READY 的时刻（用来做"断开判定冷静期"）*/
static u32 pwr_off_tmo;     /* 本轮断电要持续多久（普通 300ms / 深度 5s）*/

/* ★★ linktest 修复 v6：回显剔除改成**环形多槽**。
   v5 的单槽 echo_cmd_cur 只能装"最新一条"，但接收缓冲里往往**同时**残留
   两条我们发过的内容（`AT+CIPSEND=1,8` 命令回显 + `K,42,1` 数据帧回显），
   单槽记了新命令就丢了旧数据帧 → 数据帧回显永远剔不掉（日志里 K,42,1 一直在）。
   改为一个小环形：记最近 ECHO_RING 条发出去的内容，逐条比对剔除。*/
#define ECHO_RING   6U
#define ECHO_MAXLEN 40U
static char echo_ring[ECHO_RING][ECHO_MAXLEN];
static u16  echo_ring_len[ECHO_RING];
static u8   echo_ring_head;     /* 下一条要写的位置 */

/* 兼容旧引用：echo_cmd_cur/len 现在指向"最新一条"。*/
static char echo_cmd_cur[ECHO_MAXLEN];
static u16  echo_cmd_len;

/* ★ 两条 TCP 链路各自"连上了没"：0=PC、1=板2。
   必须记！CIPSTART=1 是**可选步**，板2 不在时那一步会被跳过；
   之后若还傻乎乎往 link1 发 CIPSEND，模块会回错、被判成"死链"，
   于是清空整个发送队列（把发往 PC 的帧一起丢）甚至整模块断电重连。
   有了这个标志：没连上的链路直接丢帧，不去踩 CIPSEND。 */
static u8  link_ok[2]     = {0U, 0U};
static u32 link_try_t0[2] = {0U, 0U};   /* 上次尝试补连该链路的时刻（限流用）*/

/* ★★★ linktest：底账计数器（只增不减，压测看增量）★★★
   这些是"链路到底断没断"的铁证 —— 不用再靠肉眼看串口刷屏。 */
static u32 tx_ok_cnt;                   /* 收到 "SEND OK" 的次数（真发出去几帧）*/
static u32 tx_busy_cnt;                 /* 模块回 busy 的次数（丢帧主因之一）*/
static u32 tx_err_cnt;                  /* 发送 ERROR / 超时 的次数 */
static u32 rx_ipd_cnt;                  /* 成功抠出 +IPD 数据的次数（收到几帧）*/
static volatile u32 rx_bytes_cnt;       /* UART4 累计实收字节（模块在不在说话）*/
static u32 cmd_cnt;                     /* 发过多少条 AT 命令 */
static u32 cmd_bytes_cnt;               /* AT 命令累计字节（反推真实波特率用）*/
static u8  txq_hi;                      /* 发送队列高水位（吞吐压力证据）*/
static u32 lt_prompt_bogus;             /* ★ linktest：非行首 '>' 被拦下的次数 */
/* link_ok 由 1 翻 0 的次数 / 本次断开起点 / 最长一次断开时长（每 slot 一份）*/
static u32 link_down_cnt[2];
static u32 link_down_t0[2];
static u32 link_down_max[2];
static u32 link_up_ms[2];               /* 在线累计毫秒（由 Wifi_LinkOk 累加）*/
static u32 link_ok_t0;                  /* 上次 Wifi_LinkOk 被调用的时刻 */

/* ★★★ linktest v7：丢帧解剖计数（只加读数，不改行为）★★★
   id 定义见 esp01s.h 的 V7_*；前 12 个由 app 层取**窗口增量**打印。 */
static u32 v7_cut;          /* 清理前 esp_line 行尾是 "+IPD," 的 1~4 字节真前缀 */
static u32 v7_clr_ipd;      /* pr==0 且行内含 "+IPD" */
static u32 v7_clr_x;        /* 任意 ClearRx 时行内含 "+IPD" */
static u32 v7_batch;        /* 单次解析抠出 ≥2 帧 */
static u32 v7_ovr;          /* 单槽覆盖（旧帧未取走就被新帧盖掉）*/
static u32 v7_strip;        /* ESP_StripEcho 实际删掉的行数 */
static u32 v7_resid_h;      /* 行首半截回显（拍级）*/
static u32 v7_resid_t;      /* 行尾半截回显（拍级）*/
static u32 v7_half;         /* stall≥20 保底清空触发 */
static volatile u32 v7_ovf; /* 环形满而丢掉的字节数（中断写）*/
static volatile u32 v7_ore; /* UART4 错误次数（中断写）*/
static volatile u32 v7_ore_code[3];   /* 前 3 次 errorcode（中断写）*/
static u32 v7_lfull;        /* esp_line 见顶且环形仍有积压 */
/* ★ v8：v7_cut / v7_clr_ipd / v7_half / v7_batch 的"埋点位置"随旧解析模型一起删除，
   四个计数从此恒为 0 —— 它们就是"旧机制已消失"的回归指示器（读数行格式不变）。
   v7_ovr 仍然有效（px_feed 里单槽覆盖照数）。*/

/* ★★★ 实验七新增：**重连原因计数**（排查"为什么老断"的第一手证据）★★★
   为什么要有它：断链在串口日志里是**一次性**的几行，人不可能一直盯着终端；
   而"老是断"到底是哪一类断，决定完全不同的排查方向：
     why_closed    READY 里看到模块吐了 "0,CLOSED" → 对端（PC）真的把连接关了，
                   或上位机重启；属于**应用层断链**，重连即可。
     why_txdead    CIPSEND 失败且判死链（"link is not valid" / "SEND FAIL"）→
                   典型的**半开连接**（对端静默消失），或模块内部把 socket 搞没了。
     why_wifilost  收到 "WIFI DISCONNECT" → **热点/路由**掉了，整模块重连。
     why_stepfail  AT 流程里某一步失败 → 整模块重连（多半是模块没答话/被复位）。
     why_deepfail  连续失败到"深断电"（5s）→ 模块**真的**不答话了：查供电/接线。
   判读经验：
     · why_closed 涨得最多   → 是 PC 那边的问题（上位机重启 / 防火墙 / 网卡休眠）
     · why_txdead 涨得最多   → 板子在发帧、但对端不见了；或模块 socket 异常
     · why_stepfail/deepfail → ★ 板子侧硬问题：**ESP 供电不足**（最常见）、
                               模块本身故障、或 CH_PD/串口线接触不良
   读数：串口敲 w。 */
static u32 why_closed;
static u32 why_txdead;
static u32 why_wifilost;
static u32 why_stepfail;
static u32 why_deepfail;
u32 Wifi_DbgWhy(u8 id)
{
    switch (id)
    {
    case 0U: return why_closed;
    case 1U: return why_txdead;
    case 2U: return why_wifilost;
    case 3U: return why_stepfail;
    case 4U: return why_deepfail;
    default: return 0U;
    }
}

/* ★ linktest：链路被判"断"时登记一笔（次数 + 起点）。
   只在 **由连变断**（link_down_t0 还是 0）时记，避免同一次断开重复计数。 */
static void lt_note_link_down(u8 slot)
{
    if (slot < 2U)
    {
        if (link_down_t0[slot] == 0U)
        {
            link_down_t0[slot] = HAL_GetTick();
            link_down_cnt[slot]++;
        }
    }
}

/* ★ linktest：链路**恢复**时结算本次断开时长（更新"最长断多久"）。 */
static void lt_note_link_up(u8 slot)
{
    if (slot < 2U)
    {
        if (link_down_t0[slot] != 0U)
        {
            u32 d = HAL_GetTick() - link_down_t0[slot];
            if (d > link_down_max[slot]) { link_down_max[slot] = d; }
            link_down_t0[slot] = 0U;
        }
    }
}
/* ★ 单发标记：1 = 当前这次 CIPSTART 是"补连"发起的，收到回显就回 READY，
   **绝不继续沿 AT 表往下走**（否则补 link0 会把 link1 一起重连、搞断）。 */
static u8  link_oneshot;

/* 周期性补连的间隔：READY 且某条链不在时，主动重试。
   ★ 不能太短：每次补连都会把状态机拉进 CONNECT 几秒（这期间发不了数据）；
     也不能太长：板2 后上电后要尽快自愈。5s 是折中。 */
#define LINK_RETRY_MS  5000U
/* PC 链的补连间隔。
   ★★ 实验六修正（原为 60000U）：那个 60s 的理由是实验五的
     "PC 只是辅助观察端、板1↔板2 才是主链路，别每 5 秒去打扰一次数据流"。
     实验六板间改走 CAN，**PC 成了唯一链路**，这个理由不再成立。
   ★ 而且必须看清：周期补连只在 `link_ok[0] == 0` 时才触发 ——
     此时链路本来就不通、Wifi_Push 本来就在丢帧，"别打扰数据流"毫无意义，
     唯一的诉求就是"PC 一起来就尽快挂上"。
   ★ 一个容易忽略的后果：CIPSTART 是**可选步**（at_opt[6]=1），
     开机时 PC 还没跑 → 这一步失败 → link_ok[0] 停在 0。
     若间隔仍是 60s，用户开着上位机也要干等一分钟才看到第一行数据。
   ⇒ 直接与 Wifi_TryLink 内部限流 LINK_RETRY_MS(5s) 同频：最多 5 秒自愈。 */
#define PC_RETRY_MS    LINK_RETRY_MS

/*---- 发送队列（每条带 link id：0=PC, 1=板2）----
 *  CIPMUX=1 下同一帧要发给 PC 和板2 两条链接，单槽装不下，用 4 深小队列；
 *  满了就丢（采样 500ms 一帧，够用）。---- */
#define TXQ_N  16U   /* 加深：补连 link1 那 2 秒的帧也要装得下 */
static u8  txq_link[TXQ_N];
static u16 txq_len[TXQ_N];
static u8  txq_data[TXQ_N][ESP_TX_SIZE];
static u8  txq_head;
static u8  txq_tail;
static u8  txq_cnt;

/*---- 优先槽：按键产生的控制帧(C,START/STOP)不能被数据帧挤在后面排队，
 *  否则"按开关到板2 灯亮"会慢半拍。单独一个槽，READY 时优先发它。
 *  同一时刻只留最后一条（灯只认最终状态，连按以最后一次为准）。---- */
static u8  urg_pending;
static u8  urg_link;
static u16 urg_len;
static u8  urg_buf[ESP_TX_SIZE];

/*---- 接收槽（+IPD 抠出的帧，单槽：新的覆盖旧的）----
 *  ★ 实验5 新增：板1 也要收板2 发来的帧（对方数据/控制）。
 *  ★★ v8：payload/plen 改由**接收中断里的状态机**写、任务里读，故加 volatile；
 *     cli_stall（"半截连续了几拍"）随旧解析模型一起删除。*/
#define CLI_PAY_MAX   80U
static volatile u8  cli_payload[CLI_PAY_MAX];
static volatile u16 cli_plen;
static volatile u8  cli_ready;

/* 正在发送的那一帧 */
static u8  tx_buf[ESP_TX_SIZE];
static u16 tx_len;
static u8  tx_link;
static volatile u8 tx_pending;      /* 1 = 有一帧正在发 */

/*==============================================================================
 *  ★★★ linktest v8：接收帧路径 = 字节流状态机（与行缓冲/清空/切片彻底解耦）★★★
 *
 *  为什么（第十一轮 v7 读数实锤，见《linktest重构v8方案.md》）：
 *    · 板2 头截断 M1 —— 每拍"搬一块再解析"，切片边界落在 "+IPD," 前 4 字节内时，
 *      解析只认完整 5 字节头 → 返回 0 → 调用方整行清空 → **整帧永久丢**。
 *      （hexdump 实证：清空时行里只有 "\r\n+IPD"）
 *    · 板1 清空吞帧 F5 —— 帧与 AT 响应共用 esp_line，发送握手（等 '>' / 等 SEND OK）
 *      期间的 ESP_ClearRx() 把已收到的 ACK 一起清掉（clrX 4~7/窗、cut=0）。
 *  两者都是"共享行缓冲 + 破坏性清空"的后果。这里把帧解析独立出来：
 *    · 每个字节直接喂状态机，状态（含"已匹配几个头字节"）跨拍/跨清空保留；
 *    · 非帧字节（回显 / OK / 提示符 / CLOSED…）一律跳过，天然无害；
 *    · 与 ESP_ClearRx / esp_ring / esp_line 完全无关 —— 清空再也吞不掉帧。
 *  喂入点在 UART 接收中断（每字节几十个周期，115200 下 <1us；中断里不碰 RTOS）。
 *============================================================================*/
#define PX_SEEK   0U
#define PX_LINK   1U
#define PX_LEN    2U
#define PX_PAY    3U

static u8  px_st = PX_SEEK;     /* 状态机当前状态 */
static u8  px_mlen;             /* "+IPD," 已匹配长度（跨字节/跨拍保留）*/
static u8  px_last;             /* 上一字节：判定"行首"用（防回显伪头）*/
static u8  px_started;          /* 流已开始（首字节无条件视为行首）*/
static u32 px_link;             /* 正在收的帧的 link 字段（板1 不关心，只跳过）*/
static u32 px_len;              /* 本帧 payload 长度 */
static u32 px_got;              /* 已收 payload 字节数 */

/* 流不连续时复位（真断链 / 模块重启 / 重新进 READY）。
   px_last/px_started 特意保留：行首判定跨复位依然成立。*/
static void px_reset(void)
{
    px_st   = PX_SEEK;
    px_mlen = 0U;
    px_link = 0U;
    px_len  = 0U;
    px_got  = 0U;
}

static void px_feed(u8 b)
{
    switch (px_st)
    {
    case PX_SEEK:
        if (px_mlen != 0U)                          /* 正在匹配 "+IPD," 前缀 */
        {
            if (b == (u8)"+IPD,"[px_mlen])
            {
                px_mlen++;
                if (px_mlen >= 5U)                  /* 5 字节齐 → 进 link 字段 */
                {
                    px_mlen = 0U;
                    px_link = 0U;
                    px_len  = 0U;
                    px_st   = PX_LINK;
                }
            }
            else
            {
                /* 失配：本字节可能正是新头的 '+'（如 "+IP+IPD," 这类重叠）*/
                px_mlen = (b == (u8)'+') ? 1U : 0U;
            }
        }
        else if ((b == (u8)'+') &&
                 ((px_started == 0U) || (px_last == 13U) || (px_last == 10U)))
        {
            px_mlen = 1U;                           /* 只认行首的 '+'（防回显伪头）*/
        }
        break;

    case PX_LINK:
        if ((b >= (u8)'0') && (b <= (u8)'9'))
        {
            px_link = px_link * 10U + (u32)(b - (u8)'0');
            if (px_link > 99U) { px_reset(); }      /* 荒谬值：放弃，重找 */
        }
        else if (b == (u8)',') { px_st = PX_LEN; }
        else { px_reset(); }
        break;

    case PX_LEN:
        if ((b >= (u8)'0') && (b <= (u8)'9'))
        {
            px_len = px_len * 10U + (u32)(b - (u8)'0');
            if (px_len > 4096U) { px_reset(); }     /* 荒谬长度：护栏 */
        }
        else if (b == (u8)':')
        {
            if (px_len == 0U) { px_reset(); }
            else { px_got = 0U; px_st = PX_PAY; }
        }
        else { px_reset(); }
        break;

    case PX_PAY:
    default:
        if (px_got < (u32)CLI_PAY_MAX)              /* 超长只截断保存 */
        {
            cli_payload[px_got] = b;
        }
        px_got++;
        if (px_got >= px_len)                       /* 收满：投递（单槽，新的覆盖旧的）*/
        {
            if (cli_ready != 0U) { v7_ovr++; }      /* 旧帧还没被取走 */
            cli_plen  = (px_len > (u32)CLI_PAY_MAX) ? (u16)CLI_PAY_MAX : (u16)px_len;
            cli_ready = 1U;
            rx_ipd_cnt++;                           /* ★ linktest：收到一帧对端数据 */
            px_reset();
        }
        break;
    }

    px_last    = b;
    px_started = 1U;
}

/*------------------------------------------------------------------------------
 *  接收中断：收一个字节塞进环形缓冲，并立刻重新武装下一次接收
 *----------------------------------------------------------------------------*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4)
    {
        u16 nxt = (u16)((esp_head + 1U) % ESP_RX_SIZE);

        px_feed(esp_rx_byte);           /* ★ v8：帧路径先喂（环形满不满都不影响它）*/

        if (nxt != esp_tail)            /* 满了就丢，不覆盖未读数据 */
        {
            esp_ring[esp_head] = esp_rx_byte;
            esp_head = nxt;
            rx_bytes_cnt++;             /* ★ linktest：累计实收字节 */
        }
        else
        {
            v7_ovf++;                   /* ★ v7：环形满 → 丢字节（底层丢帧证据）*/
        }
        (void)HAL_UART_Receive_IT(&huart4, &esp_rx_byte, 1U);
    }
}

/*------------------------------------------------------------------------------
 *  出错回调：发生溢出(ORE)/帧错误后，HAL 会把接收中断关掉。
 *  不在这里重新挂上，接收就永久"死"了 —— 现象是后面再也看不到模块说话
 *  （日志里出现 "ERROR CLO" 这种半截回复就是它）。所以无脑重新武装一次。
 *----------------------------------------------------------------------------*/
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4)
    {
        /* ★ v7：只统计，不动行为（重新武装仍然是唯一动作）*/
        v7_ore++;
        if (v7_ore <= 3U)
        {
            v7_ore_code[v7_ore - 1U] = huart->ErrorCode;
        }
        (void)HAL_UART_Receive_IT(&huart4, &esp_rx_byte, 1U);
    }
}

/*==============================================================================
 *  底层小工具
 *============================================================================*/

/* ★ linktest：回显探测的前置声明（定义在文件后面，实现见 Wifi_Dbg* 区）*/
void Wifi_DbgNoteCmd(const u8 *buf, u16 len);
static void lt_check_echo(void);

/*------------------------------------------------------------------------------
 *  ★★ linktest 修复 v2：**软件侧剔除"命令回显行"**（治本，不依赖模块）★★
 *
 *  第五轮日志铁证：这块 ESP-01 固件**根本不吃 ATE0** —— 补发的 ATE0 照样被
 *  原样回显（`>> ATE0` 紧跟 `<< ATE0`），echo 计数一路涨。
 *  既然模块不配合，就由**我们**在解析前把回显行丢掉：
 *
 *    esp_line 形如（发 "ATE0"）：  "ATE0\r\n\r\nOK\r\n"
 *    我们要把 "ATE0\r\n" 这一段（我们刚发出去的命令）删掉，
 *    只留模块的真回复。
 *
 *  做法：拿 echo_cmd_cur（ESP_Begin 记下的"当前命令"），在 esp_line 里
 *  逐行扫；某行**完全等于**该命令时，连同它后面的一个空行/CRLF 一起删除。
 *  原地 memmove 收尾。只删"整行完全相同"的行 —— 绝不会误删模块回复
 *  （模块回复是 OK/ERROR/CONNECT 之类，不会等于我们发的 AT 命令）。
 *
 *  ★ 为什么只删"当前命令"的行：esp_line 里可能同时留着好几条命令的回显，
 *    只删最新那条也够用（旧的会被 ESP_ClearRx 在各状态切换时倒掉）。
 *----------------------------------------------------------------------------*/
static void ESP_StripEcho(void)
{
    u8 pass;
    u8 changed = 1U;

    /* ★ v6：对环形里每一条"我们发过的内容"都扫一遍。
       新删掉的内容会让后面的内容前移，所以多跑几轮直到一轮下来没变化
       （最多 4 轮，条目有限，必然收敛）。*/
    for (pass = 0U; (pass < 4U) && (changed != 0U); pass++)
    {
        u8 s;
        changed = 0U;
        for (s = 0U; s < ECHO_RING; s++)
        {
            u16 nlen = echo_ring_len[s];
            u16 i = 0U;

            if (nlen == 0U) { continue; }

            while ((u16)(i + nlen) <= esp_len)
            {
                if ((i != 0U) && (esp_line[i - 1U] != 13U) && (esp_line[i - 1U] != 10U))
                {
                    i++;
                    continue;
                }
                if (memcmp(&esp_line[i], echo_ring[s], (u32)nlen) != 0)
                {
                    i++;
                    continue;
                }
                {
                    u16 j = (u16)(i + nlen);
                    if ((j < esp_len) && (esp_line[j] != 13U) && (esp_line[j] != 10U))
                    {
                        i++;                /* 后面粘着别的字符：不是整行匹配 */
                        continue;
                    }
                    /* 命中：吃掉这一行（含 CR/LF），再顺带吃掉紧随的空行 */
                    while ((j < esp_len) && ((esp_line[j] == 13U) || (esp_line[j] == 10U)))
                    {
                        j++;
                    }
                    {
                        u16 k = j;
                        while ((k < esp_len) && ((esp_line[k] == 13U) || (esp_line[k] == 10U)))
                        {
                            k++;
                        }
                        if ((k > j) && (k < esp_len)) { j = k; }
                    }
                    {
                        u16 n = (u16)(esp_len - j);
                        u16 m;
                        for (m = 0U; m < n; m++) { esp_line[i + m] = esp_line[j + m]; }
                        esp_len = (u16)(i + n);
                        esp_line[esp_len] = 0U;
                    }
                    v7_strip++;         /* ★ v7：真的删掉了一条回显行 */
                    changed = 1U;
                }
            }
        }
    }
}

/*------------------------------------------------------------------------------
 *  ★ v7 诊断：半截回显残留检测（StripEcho 之后，看一眼 esp_line 的首/尾）
 *    行首 6 字节 == 某条 echo 条目首 6 字节 → resid_h
 *    行尾 6 字节 == 某条 echo 条目尾 6 字节 → resid_t
 *    拍级布尔（每拍每类至多 +1）；成本 = ECHO_RING×2 次 memcmp(6)。
 *----------------------------------------------------------------------------*/
static void v7_check_resid(void)
{
    u8 s;
    u8 h = 0U;
    u8 t = 0U;

    if (esp_len < 6U) { return; }

    for (s = 0U; s < ECHO_RING; s++)
    {
        u16 nlen = echo_ring_len[s];

        if (nlen < 6U) { continue; }
        if ((h == 0U) && (memcmp(esp_line, echo_ring[s], 6U) == 0))
        {
            h = 1U;
        }
        if ((t == 0U) && (memcmp(&esp_line[esp_len - 6U], &echo_ring[s][nlen - 6U], 6U) == 0))
        {
            t = 1U;
        }
    }
    v7_resid_h += (u32)h;
    v7_resid_t += (u32)t;
}

/* 把环形缓冲里的新数据搬到 esp_line，方便做字符串查找 */
static void ESP_Collect(void)
{
    while ((esp_tail != esp_head) && (esp_len < (ESP_LINE_SIZE - 1U)))
    {
        esp_line[esp_len] = esp_ring[esp_tail];
        esp_len++;
        esp_tail = (u16)((esp_tail + 1U) % ESP_RX_SIZE);
    }
    if ((esp_tail != esp_head) && (esp_len >= (ESP_LINE_SIZE - 1U)))
    {
        v7_lfull++;             /* ★ v7：见顶且环形还有积压 → 字节被卡在环里 */
    }
    esp_line[esp_len] = 0U;

    lt_check_echo();        /* ★ linktest：查一眼"收到的里面有没有自己刚发的"（删除前） */
    ESP_StripEcho();        /* ★ linktest v2：把命令回显行从 esp_line 里剔除 */
    v7_check_resid();       /* ★ v7：剔除后仍残留的半截回显 */
}

/* 在 esp_line 里找子串，找到返回 1 */
static u8 ESP_Find(const char *s)
{
    u16 n = (u16)strlen(s);
    u16 i;
    u16 j;

    if ((n == 0U) || (esp_len < n))
    {
        return 0U;
    }
    for (i = 0U; (u16)(i + n) <= esp_len; i++)
    {
        for (j = 0U; j < n; j++)
        {
            if (esp_line[i + j] != (u8)s[j])
            {
                break;
            }
        }
        if (j == n)
        {
            return 1U;
        }
    }
    return 0U;
}

/*------------------------------------------------------------------------------
 *  ★★ linktest 修复（回显版）：**跳过"我们自己发出去的那一行"** ★★
 *
 *  背景（第四轮压测日志铁证）：模块的 ATE0 没真正生效（或上电默认回显开着），
 *  我们发的每条命令都会先被**原样回显**回来，之后才是模块的真回复。
 *  于是 esp_line 长这样（发 "ATE0"）：
 *       ATE0\r\n       <- 回显（我们自己发的）
 *       OK\r\n         <- 模块真回复
 *
 *  后果有两处：
 *    1) 等 "OK" 本身没事；但如果等待的关键字**恰好是自己的命令**
 *       （比如 AT+CIPSTART 等 "OK" 没问题，但 "ALREADY CONNECTED" 之类的
 *        判断、以及任意一条**前缀相同**的等待），回显就会**提前命中**；
 *    2) 更致命的是「+IPD / CLOSED 的判据」被回显污染 —— 见 ParseClient。
 *
 *  这里给出一个通用工具：给一个**我们刚发出去的命令字节**，
 *  在 esp_line 里找出"回显行"的范围，后续判据可以据此跳过。
 *  为了控制改动面，本函数只在 ESP_Poll 里用于"判定 OK 时排除回显行"。
 *
 *  返回 1 = 在行首找到了与 cmd 完全相同的回显行。
 *----------------------------------------------------------------------------*/
static u8 ESP_FindEchoLine(const char *cmd)
{
    u16 n = (u16)strlen(cmd);
    u16 i;

    if ((n == 0U) || (esp_len < n))
    {
        return 0U;
    }
    for (i = 0U; (u16)(i + n) <= esp_len; i++)
    {
        /* 只在行首比对整行（回显永远独占一行、且是完整命令） */
        if ((i != 0U) && (esp_line[i - 1U] != 13U) && (esp_line[i - 1U] != 10U))
        {
            continue;
        }
        if (memcmp(&esp_line[i], cmd, (u32)n) == 0)
        {
            return 1U;
        }
    }
    return 0U;
}

/*------------------------------------------------------------------------------
 *  ★★ linktest 修复（回显版）：把"回显行"从比对里剔除后的查找 ★★
 *
 *  语义：在 esp_line 里找子串 s，但**跳过所有与 echo_cmd 相同的行**。
 *  典型用法：等 "OK"，但命令是 "ATE0"（回显行也是 "ATE0"，不含 OK，
 *  所以其实不冲突）—— 真正冲突的是**等待关键字与命令本身有重叠**的情况，
 *  例如发 "AT" 等 "OK"（AT 是 OK 的子串？不是）……
 *  更常见的冲突是：发 "AT+CIPSEND=0,20"，等提示符 ">"（独立行，不冲突）。
 *
 *  ★ 但确有一类真实冲突：模块回显里包含 "OK" 字样时（如 "SEND OK"），
 *    以及 ATE0 之前模块的出厂回显 —— 为稳妥起见提供本函数，
 *    在 ESP_Poll 里凡命中关键字都再过一遍"是否只出现在回显行里"，
 *    若是则视为未命中，继续等。
 *----------------------------------------------------------------------------*/
static u8 ESP_FindSkipEcho(const char *s, const char *echo_cmd)
{
    u16 n = (u16)strlen(s);
    u16 en = (u16)strlen(echo_cmd);
    u16 i;

    if ((n == 0U) || (esp_len < n))
    {
        return 0U;
    }
    for (i = 0U; (u16)(i + n) <= esp_len; i++)
    {
        u16 j;
        u8  in_echo = 0U;

        for (j = 0U; j < n; j++)
        {
            if (esp_line[i + j] != (u8)s[j]) { break; }
        }
        if (j != n) { continue; }           /* 这个位置不是 s */

        /* 命中位置 i 是否落在"回显行"里？回显行 = 以行首开始、长度 en 的 cmd */
        if ((en != 0U) && ((u16)(i + 1U) >= en))
        {
            u16 start = (u16)(i + 1U - en);
            /* start 必须是行首，且 start..start+en 与 cmd 完全相同，
               且命中位置 i 在 [start, start+en) 之内 */
            if (((start == 0U) || (esp_line[start - 1U] == 13U) ||
                 (esp_line[start - 1U] == 10U)) &&
                (memcmp(&esp_line[start], echo_cmd, (u32)en) == 0))
            {
                in_echo = 1U;
            }
        }
        if (in_echo == 0U)
        {
            return 1U;                      /* 命中且不在回显行 -> 真命中 */
        }
    }
    return 0U;
}

/*------------------------------------------------------------------------------
 *  ★★★ 修复：只在"行首"找 CIPSEND 的 ">" 提示符 ★★★
 *
 *  血泪教训（实测日志）：原来等提示符用的是 ESP_Find(">") —— 在**整条**
 *  esp_line 缓冲里瞎找 '>' 字符。而模块的正式提示符是**独占一行的 ">"**
 *  （前面是 \r\n）。只要缓冲里**任何地方**出现 '>'，就会被误判成"可以发了"：
 *      - PC 链(reset) 补连的残留回显（"ERROR\r\n0,CLOSED" 之后若粘着东西）；
 *      - 对方发来的 +IPD 数据里恰好带 '>'；
 *      - 上一轮 SEND 的回显没清干净。
 *  一旦误判，就会**提前**把数据吐出去 —— 模块此时还没进接收态，
 *  把它当命令吃掉，回 "Recv N bytes"，这一帧**彻底丢失**（日志里就是这么丢的）。
 *
 *  本函数只认：esp_line 当前位置是 '>'，且它前面是开头或 \r/\n。
 *  返回 1 = 找到合法的提示符。
 *----------------------------------------------------------------------------*/
static u8 ESP_FindPrompt(void)
{
    u16 i;

    for (i = 0U; i < esp_len; i++)
    {
        if (esp_line[i] != (u8)('>'))
        {
            continue;
        }
        /* '>' 必须在行首：i==0，或前一个字符是 \r / \n */
        if ((i == 0U) ||
            (esp_line[i - 1U] == (u8)(13)) ||
            (esp_line[i - 1U] == (u8)(10)))
        {
            return 1U;
        }
        /* ★ linktest：发现一个"非法位置"的 '>' —— 这正是老 bug 的肇事者。
           老代码在这里就误判成"可以发了"，把数据提前吐出去 → Recv N bytes。
           现在不认它，只记数，用来证明修复是否真的堵住了这条路。 */
        lt_prompt_bogus++;
    }
    return 0U;
}

static void ESP_SendRaw(const u8 *buf, u16 len)
{
    (void)HAL_UART_Transmit(&huart4, (u8 *)buf, len, 1000U);
}

static void ESP_ClearRx(void)
{
    /* ★ v7：清空前登记"这一清会不会清掉帧相关字节"（含 +IPD 字样）。
       任意调用点（补连/断链/CIPSEND 前）都算 —— M2 观测口。*/
    if ((esp_len > 0U) && (ESP_Find("+IPD") != 0U))
    {
        v7_clr_x++;
    }
    esp_tail = esp_head;
    esp_len  = 0U;
    esp_line[0] = 0U;
}

/* 把 esp_line 当前内容打出来（一条指令的完整回显） */
static void ESP_ShowLine(const char *tag)
{
    UART_Printf("[WiFi] %s %s%c%c", tag, esp_line, 13U, 10U);
}

/*------------------------------------------------------------------------------
 *  失败时把收到的原始字节按 hex 打出来 —— 用来分清到底出了哪种情况：
 *      len=0            : 模块一个字都没回       -> 模块没工作 / 没供电 / 线断了
 *      len>0 且全乱码   : 模块在说话但被干扰     -> 走线串扰 / 电源噪声 / 地 / 射频
 *      能看出 ASCII     : 模块回了具体内容（如 FAIL / busy 等），是被逻辑忽略了
 *  最多打 64 字节，每行 16 个。（UART_Printf 每行有长度上限，所以分行打）
 *----------------------------------------------------------------------------*/
static void ESP_ShowHex(const char *tag)
{
    u16 i;
    u16 n = esp_len;

    if (n > 64U)
    {
        n = 64U;
    }

    UART_Printf("[WiFi] %s len=%u (hex)%c%c", tag, (u32)esp_len, 13U, 10U);

    for (i = 0U; i < n; i += 16U)
    {
        u16 j;
        UART_Puts("[WiFi]   ");
        for (j = i; (j < (u16)(i + 16U)) && (j < n); j++)
        {
            UART_Printf("%02X ", (u32)esp_line[j]);
        }
        UART_Printf("%c%c", 13U, 10U);
    }
}

/*------------------------------------------------------------------------------
 *  CH_PD(PF14)：拉低 = 模块彻底断电，拉高 = 模块启动。
 *  ★ 引脚本身（GPIOF 时钟 + 输出模式 + 初值低）现在由 CubeMX 生成的
 *    Core/Src/gpio.c 里 MX_GPIO_Init() 配好（.ioc: PF14 = GPIO_Output / Low），
 *    这里只保留"拉高/拉低"的动作，不再自己配 GPIO。
 *----------------------------------------------------------------------------*/
static void ESP_EnWrite(GPIO_PinState s)
{
    HAL_GPIO_WritePin(ESP_EN_PORT, ESP_EN_PIN, s);
}

/*------------------------------------------------------------------------------
 *  给 UART4_RX(PA1) 打开内部上拉。
 *
 *  为什么：模块没工作/正在复位时，它的 TX 引脚是高阻，我们这一侧 RX 若悬空
 *  就会拾取噪声 —— 现象是 AT 超时时收到满缓冲的垃圾（len=511），把真正有用的
 *  日志全淹了。开了上拉，模块不说话时 RX 稳定读空闲高电平，既不再灌垃圾，
 *  也能让我们分清"模块真的没说话"和"线噪声"。
 *  （HAL 的 MspInit 里 PA0/PA1 是 NOPULL，所以这里必须自己补一次。）
 *----------------------------------------------------------------------------*/
static void ESP_UartRxPullUp(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gi.Pin       = GPIO_PIN_1;              /* UART4_RX */
    gi.Mode      = GPIO_MODE_AF_PP;
    gi.Pull      = GPIO_PULLUP;             /* 关键：防悬空拾噪 */
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOA, &gi);
}

/*------------------------------------------------------------------------------
 *  ★★ linktest 修复 v5：设置"当前要剔除的回显内容"（AT 命令 或 数据帧）。
 *
 *  血泪教训（第八轮日志）：AT+CIPSEND 的数据帧本身**也会被模块回显**！
 *  板1 发 `K,29,1\r\n` → 模块吐回 `K,29,1\r\n` → 落进 esp_line。
 *  v5 只记最新一条（会被下一条命令覆盖），数据帧回显仍剔不掉。
 *  v6 改环形：最近 ECHO_RING 条都记着，ESP_StripEcho 逐条比对剔除。
 *----------------------------------------------------------------------------*/
static void ESP_SetEchoCmd(const u8 *buf, u16 len)
{
    u16 ci = 0U;
    u8  slot = echo_ring_head;

    while ((ci < len) && (ci < (u16)(ECHO_MAXLEN - 1U)))
    {
        echo_ring[slot][ci] = (char)buf[ci];
        ci++;
    }
    echo_ring[slot][ci] = (char)0;
    echo_ring_len[slot] = ci;
    echo_ring_head = (u8)((echo_ring_head + 1U) % ECHO_RING);

    /* 兼容：echo_cmd_cur 指向最新一条 */
    for (ci = 0U; ci <= echo_ring_len[slot]; ci++) { echo_cmd_cur[ci] = echo_ring[slot][ci]; }
    echo_cmd_len = echo_ring_len[slot];
}

/*------------------------------------------------------------------------------
 *  发一条 AT 指令（只发，不等）
 *----------------------------------------------------------------------------*/
static void ESP_Begin(const char *cmd, u32 timeout)
{
    ESP_ClearRx();

    cmd_cnt++;                          /* ★ linktest：命令条数 */
    cmd_bytes_cnt += (u32)strlen(cmd) + 2U;   /* +CRLF：反推真实波特率用 */

    /* ★ linktest 修复（回显版）：记住"当前在等回应的这条命令"。
       ESP_Poll 里判定关键字会用 ESP_FindSkipEcho 把它的回显行剔除，
       避免"命令回显"被当成"模块回应"（ATE0 失效时尤其致命）。*/
    ESP_SetEchoCmd((const u8 *)cmd, (u16)strlen(cmd));

    UART_Printf("[WiFi] >> %s%c%c", cmd, 13U, 10U);
    ESP_SendRaw((const u8 *)cmd, (u16)strlen(cmd));
    ESP_SendRaw(esp_crlf, 2U);

    Wifi_DbgNoteCmd((const u8 *)cmd, (u16)strlen(cmd));   /* ★ 回显探测：记下这条 */

    step_t0  = HAL_GetTick();
    step_tmo = timeout;
}

/*------------------------------------------------------------------------------
 *  查一眼回应（只查，不等）
 *      返回 1 = 等到期望关键字
 *      返回 2 = 模块报 ERROR，或者超时了
 *      返回 0 = 还没来，下次再查
 *----------------------------------------------------------------------------*/
static u8 ESP_Poll(const char *expect)
{
    ESP_Collect();

    /* ★ 有些"ERROR"其实是"已经是想要的状态"：CIPMUX 会回 "link is builded"、
       CIPSTART 会回 "ALREADY CONNECTED"。当成功处理 —— 否则重连时会把
       一个没必要的错误升级成"整模块断电重连"。 */
    if ((ESP_Find("link is builded") != 0U) ||
        (ESP_Find("ALREADY CONNECTED") != 0U))
    {
        ESP_ShowLine("<< (already in that state)");
        return 1U;
    }

    if (ESP_Find("ERROR") != 0U)
    {
        ESP_ShowLine("<< ERROR:");
        ESP_ShowHex("<< raw");
        return 2U;
    }
    /* CWJAP 连不上热点时回的是 "FAIL"（不含 ERROR），单独认一下，
       否则会被当成 TIMEOUT，看着像"没回应"，其实模块明确报了失败。 */
    if (ESP_Find("FAIL") != 0U)
    {
        ESP_ShowLine("<< FAIL:");
        ESP_ShowHex("<< raw");
        return 2U;
    }
    if (ESP_Find(expect) != 0U)
    {
        /* ★ linktest 修复（回显版）：命中的 expect 可能只是**命令回显**的
           一部分（ATE0 失效时）。用 ESP_FindSkipEcho 排除"当前命令的回显行"，
           只有真正出现在回显行**之外**才认。 */
        if ((echo_cmd_len == 0U) ||
            (ESP_FindSkipEcho(expect, echo_cmd_cur) != 0U) ||
            (ESP_FindEchoLine(expect) == 0U))
        {
            ESP_ShowLine("<<");
            return 1U;
        }
    }
    if ((HAL_GetTick() - step_t0) >= step_tmo)
    {
        ESP_ShowLine("<< TIMEOUT:");
        ESP_ShowHex("<< raw");
        return 2U;
    }
    return 0U;
}

/*------------------------------------------------------------------------------
 *  拼一条 AT+CIPSEND=<link>,<len>（CIPMUX=1 发送数据前要指定 link）
 *----------------------------------------------------------------------------*/
static void ESP_BeginCipSend(u8 link, u16 len)
{
    u8  cmd[24];
    u16 k = 0U;
    u8  tmp[6];
    u16 dn = 0U;
    u16 v = len;
    u16 i;

    /* "AT+CIPSEND=" */
    {
        const char *p = "AT+CIPSEND=";
        while (*p != (char)0)
        {
            cmd[k] = (u8)(*p);
            k++;
            p++;
        }
    }
    /* link 一位数字 + ',' */
    cmd[k] = (u8)(48U + (link % 10U)); k++;
    cmd[k] = (u8)(','); k++;

    /* len 十进制 */
    if (v == 0U)
    {
        tmp[dn] = (u8)(48);
        dn++;
    }
    else
    {
        while ((v != 0U) && (dn < 6U))
        {
            tmp[dn] = (u8)(48U + (v % 10U));
            v = (u16)(v / 10U);
            dn++;
        }
    }
    for (i = 0U; i < dn; i++)
    {
        cmd[k] = tmp[dn - 1U - i];
        k++;
    }
    cmd[k] = 0U;

    /* 故意不打印（每 500ms 一帧，会淹掉日志）；出问题时 TX 分支会打回显。*/
    ESP_ClearRx();
    ESP_SendRaw(cmd, k);
    ESP_SendRaw(esp_crlf, 2U);
}

/*------------------------------------------------------------------------------
 *  掉线：歇一会儿再整个重来（含模块断电重启）
 *
 *  连着失败好几次，说明模块是真卡死了 —— 这时候有两种死法：
 *      a) 卡在"假死"状态：CH_PD 拉低 300ms 就够，能自己缓过来；
 *      b) 卡在不上不下的状态：300ms 根本没用，得断电久一点。
 *  所以第 ESP_RETRY_DEEP 次之后改用 5 秒深度断电，并提示人去拔一下电。
 *----------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
 *  在回显里找"某条链路被关闭"：模块是按 link 报的 —— "0,CLOSED" / "1,CLOSED"。
 *  ★ 不能只找通用 "CLOSED"：板2(link1) 断开时会把好好的 PC(link0) 链路
 *    一起当成"死了"，触发一次没必要的 TCP 重连。
 *
 *  ★★ 实验5 重要修正：**必须跳过 +IPD 里的载荷字节**！
 *     对端发来的数据帧形如  +IPD,1,20:D,1,...  或  +IPD,1,12:P,HELLO,1..
 *     载荷是任意 ASCII，万一里面凑巧出现 "1,CLOSED" 字样，
 *     ESP_Find() 就会隔着 +IPD 头把它找到，误判成"链路断了"
 *     → 于是两边互相认为对方掉线、各自重连 → **无限断开/重连抖动**。
 *     所以这里改成"逐行扫"：遇到 +IPD 头就按声明长度**跳过整段载荷**，
 *     只在真正的模块回显（行首）里找  <link>,CLOSED。
 *----------------------------------------------------------------------------*/
static u8 ESP_FindLinkClosed(u8 link)
{
    u16 i = 0U;
    u8  d = (u8)('0' + (link % 10U));

    while (i < esp_len)
    {
        /* 行首碰到 +IPD：解析出 link / len，把整段（头+载荷）跳过 */
        if ((u16)(i + 5U) <= esp_len &&
            esp_line[i] == (u8)'+' && esp_line[i + 1U] == (u8)'I' &&
            esp_line[i + 2U] == (u8)'P' && esp_line[i + 3U] == (u8)'D' &&
            esp_line[i + 4U] == (u8)',' )
        {
            u16 j = (u16)(i + 5U);
            u32 len = 0U;
            u8  phase = 0U;
            u8  sep = 0U;

            while (j < esp_len)
            {
                u8 ch = esp_line[j];
                if ((ch >= (u8)'0') && (ch <= (u8)'9'))
                {
                    if (phase != 0U) { len = len * 10U + (u32)(ch - (u8)'0'); }
                    j++;
                }
                else if (ch == (u8)',') { phase = 1U; j++; }
                else if (ch == (u8)':') { j++; sep = 1U; break; }
                else { break; }
            }
            if (sep == 0U) { break; }           /* 头没收全：本次不判 */
            i = (u16)(j + (u16)len);            /* ★ 跳过整段载荷 */
            continue;
        }

        /* 真正的回显：行首（或紧跟在 CR/LF 之后）出现 <link>,CLOSED */
        if ((esp_line[i] == d) && ((u16)(i + 8U) <= esp_len) &&
            (esp_line[i + 1U] == (u8)',') &&
            (esp_line[i + 2U] == (u8)'C') && (esp_line[i + 3U] == (u8)'L') &&
            (esp_line[i + 4U] == (u8)'O') && (esp_line[i + 5U] == (u8)'S') &&
            (esp_line[i + 6U] == (u8)'E') && (esp_line[i + 7U] == (u8)'D'))
        {
            /* 前面必须是行首 / CR / LF（防 "+IPD,1,CLOSEDx" 之类贴着出现）*/
            if ((i == 0U) || (esp_line[i - 1U] == 13U) || (esp_line[i - 1U] == 10U))
            {
                return 1U;
            }
        }
        i++;
    }
    return 0U;
}

/* ★ v8：旧的"整行解析"（ESP_ParseClient）已删除 —— 帧解析改由 px_feed()
   在 UART 接收中断里逐字节完成（见文件上方"字节流状态机"）。
   它曾带来两类丢帧（第十一轮 v7 读数实锤）：
     ① 切片边界落在 "+IPD," 前 4 字节内时，它只认完整 5 字节头 → 报"没找到"
        → 调用方整行清空 → **整帧永久丢**；
     ② 它与 AT 响应共用 esp_line —— 发送握手（等 '>' / 等 SEND OK）期间的
        ESP_ClearRx() 会把已收到的帧一起清掉。 */

/* 取一条收到的帧（供 app 层消费）。语义同板2 的 Wifi_GetRx。 */
u8 Wifi_GetRx(u8 *out, u16 max)
{
    u16 i;

    if (cli_ready == 0U)
    {
        return 0U;
    }
    cli_ready = 0U;

    for (i = 0U; (i < cli_plen) && (i < max); i++)
    {
        out[i] = cli_payload[i];
    }
    /* ★ v7 附修（打印层）：补字符串结束符。
       原来只由调用方 rb[sizeof(rb)-1]='\0' 封尾 —— 载荷短（8B）而缓冲长（80B）
       时，%s 会一路读到栈残留，日志里就出现莫名的裸行（如 ACK 后面跟一行
       "K,<n>,1"、学号尾 "111"）。这里按实际长度收尾，日志才说真话。*/
    if (i < max)
    {
        out[i] = 0U;
    }
    return 1U;
}

static void Wifi_Retry(void)
{
    ESP_ClearRx();
    px_reset();                         /* ★ v8：模块重来 → 帧流不连续，复位状态机 */

    /* ★ 链路断了：丢掉发送队列里的旧帧。断电重连要好几秒，
       留着它们会在重连后一股脑发出去 —— 上位机就收到几十秒前的老数据了。*/
    txq_cnt     = 0U;
    txq_head    = 0U;
    txq_tail    = 0U;
    urg_pending = 0U;
    tx_pending  = 0U;
    lt_note_link_down(0U);              /* ★ linktest：整模块重来也记断开 */
    lt_note_link_down(1U);
    link_ok[0]  = 0U;                   /* 链路全断，状态一起清 */
    link_ok[1]  = 0U;
    link_oneshot = 0U;                  /* 整模块重来：清掉补连单发标记 */

    step_t0 = HAL_GetTick();

    if (retry_count < 250U)
    {
        retry_count++;
    }
    if (retry_count >= ESP_RETRY_DEEP)
    {
        why_deepfail++;
        pwr_off_tmo = ESP_TMO_PWR_OFF_DEEP;
        if (retry_count == ESP_RETRY_DEEP)
        {
            UART_Printf("[WiFi] module keeps not answering -> deep "
                        "power-off. If it still never comes back, pull the "
                        "board's USB out and plug it back in.%c%c", 13U, 10U);
        }
    }
    else
    {
        pwr_off_tmo = ESP_TMO_PWR_OFF;
    }

    wifi_state = WIFI_ST_RETRY;
}

/*------------------------------------------------------------------------------
 *  单独把某条链路的 CIPSTART 重走一次（**不碰另一条、不清发送队列**）。
 *  返回 1 = 已经连着；0 = 已发起 / 暂时不能发（忙、或刚试过在限流窗口内）。
 *
 *  用途：实验5 两条链路(link0=PC、link1=板2)都是可选步 —— 开机时谁不在就先跳过；
 *  等它以后上线了，靠 **READY 里的周期性补连**（LINK_RETRY_MS）或按控制键
 *  （Wifi_PushUrgent）把它单独挂回来。
 *  限流 LINK_RETRY_MS 一次：与周期补连同频，既够快自愈，又不会反复进连接态
 *  打扰发数据（每次补连要把状态机拉进 CONNECT 几秒，那期间发不了帧）。
 *
 *  ★★ 关键：这是一次**单发**操作 —— 只发这一条 CIPSTART，收到回显就回 READY，
 *     **绝不沿 AT 表往下走**！
 *     早期版本让 step_idx 继续 ++，结果补连 link0(PC) 时会顺带把
 *     link1(板2) 也重连一遍；而模块正忙/旧链还在，于是回 "busy p..." /
 *     "ALREADY CONNECTED" / "1,CLOSED"，**把好好的板2 链路一起搞断**
 *     → 表现为链路每几秒抖一次（实测日志）。用 link_oneshot 标记隔离开。
 *----------------------------------------------------------------------------*/
static u8 Wifi_TryLink(u8 link)
{
    /* ★ 实验六：三块板都只连 PC 一条链路（link0）。板间数据走 CAN，
       不再有"对板 link1"，所以 link != 0 的补连请求直接拒绝 ——
       否则会去踩一个流程表里已经不存在的 CIPSTART 步骤。 */
    if (link != 0U)
    {
        return 0U;
    }
    if (link_ok[link] != 0U)
    {
        return 1U;                      /* 已经连着 */
    }
    if (wifi_state != WIFI_ST_READY)
    {
        return 0U;                      /* 正在连接/发送中，下次再说 */
    }
    if ((HAL_GetTick() - link_try_t0[link]) < LINK_RETRY_MS)
    {
        return 0U;                      /* 刚试过，先缓一缓 */
    }
    link_try_t0[link] = HAL_GetTick();

    step_idx = (link == 1U) ? AT_STEP_CIPSTART1 : AT_STEP_CIPSTART0;
    step_t0  = HAL_GetTick();
    link_oneshot = 1U;                    /* ★ 只走这一步，完事回 READY */
    ESP_Begin(at_cmd[step_idx], 2000U);   /* 后台补连：只等 2s，别耽误发数据 */
    wifi_state = WIFI_ST_CMD;
    UART_Printf("[WiFi] re-CIPSTART link%u ...%c%c", (u32)link, 13U, 10U);
    return 0U;
}

/*==============================================================================
 *  对外接口
 *============================================================================*/

/*------------------------------------------------------------------------------
 *  开始后台联网。App_Init() 里调一次，之后全靠 Wifi_Tick() 推进。
 *  这一步不做任何等待，立刻返回 —— 这就是和旧版最大的区别。
 *----------------------------------------------------------------------------*/
void Wifi_Start(void)
{
    if (wifi_state != WIFI_ST_OFF)
    {
        return;
    }

    if (uart_ready == 0U)
    {
        MX_UART4_Init();            /* UART4 由 main.c 的 MX_UART4_Init() 初始化；兜底再调一次 */
        uart_ready = 1U;
    }

    ESP_UartRxPullUp();             /* RX 上拉：防模块停摆时 RX 悬空灌噪声 */

    ESP_EnWrite(ESP_EN_OFF);        /* 先断电，让模块干净启动 */
    step_t0      = HAL_GetTick();
    retry_count  = 0U;
    pwr_off_tmo  = ESP_TMO_PWR_OFF;
    wifi_state   = WIFI_ST_PWR_LOW;

    UART_Printf("%c%c[WiFi] background link-up started (CH_PD = PF14)%c%c",
                13U, 10U, 13U, 10U);
}

/*------------------------------------------------------------------------------
 *  主循环里每次调一次，推进一步。单个调用耗时 = 零（除了发一串字节）。
 *----------------------------------------------------------------------------*/
void Wifi_Tick(void)
{
    u8  r;

    switch (wifi_state)
    {
    /* ---- 模块断电中 ---- */
    case WIFI_ST_PWR_LOW:
        if ((HAL_GetTick() - step_t0) < pwr_off_tmo)
        {
            break;
        }
        ESP_EnWrite(ESP_EN_ON);             /* 上电，模块开始启动 */
        ESP_ClearRx();
        (void)HAL_UART_Receive_IT(&huart4, &esp_rx_byte, 1U);
        step_t0 = HAL_GetTick();
        wifi_state = WIFI_ST_PWR_WAIT;
        break;

    /* ---- 等模块启动（1.5 秒，期间主循环照转）---- */
    case WIFI_ST_PWR_WAIT:
        if ((HAL_GetTick() - step_t0) < ESP_TMO_PWR_ON)
        {
            break;
        }
        step_idx = 0U;
        at_try   = 0U;
        link_oneshot = 0U;                  /* 整表从头走：清掉补连单发标记 */
        ESP_Begin(at_cmd[0], (u32)at_tmo[0]);
        wifi_state = WIFI_ST_CMD;
        break;

    /* ---- 等某条 AT 的回显 ---- */
    case WIFI_ST_CMD:
        r = ESP_Poll("OK");
        if (r == 0U)
        {
            break;
        }
        if (r == 2U)
        {
            /* 刚上电时模块偶尔还在忙，回一句 "busy p..." 而不是 OK。
               第一条 AT 先就地重试几次 —— 比把整个模块断电重来快得多
               （断电重来要走 POWER 300ms + BOOT 1500ms + 重连全套）。*/
            if ((step_idx == 0U) && (at_try < ESP_AT_RETRY_MAX))
            {
                at_try++;
                UART_Printf("[WiFi] AT busy, in-place retry %u/%u%c%c",
                            (u32)at_try, (u32)ESP_AT_RETRY_MAX, 13U, 10U);
                ESP_Begin(at_cmd[0], (u32)at_tmo[0]);
                break;
            }
            /* 可选步（link0=PC / link1=板2 都是）：失败也继续，不把整个模块重连 */
            if (at_opt[step_idx] == 0U)
            {
                wifi_err = (u8)(step_idx + 1U);     /* 卡在第几步 */
                why_stepfail++;
                UART_Printf("[WiFi] step %u/%u failed -> retry%c%c",
                            (u32)(step_idx + 1U), (u32)AT_COUNT, 13U, 10U);
                Wifi_Retry();
                break;
            }
            if (step_idx == AT_STEP_CIPSTART0)
            {
                lt_note_link_down(0U);      /* ★ linktest：记一笔断开 */
                link_ok[0] = 0U;            /* ★ PC 不在：标记掉，别再去踩 link0 */
                /* ★ 打上补连时间戳：让 PC_RETRY_MS 限流对"主流程失败"也生效。
                   否则 READY 里的 `0,CLOSED` 检测会紧接着又发起一次补连，
                   两次补连挨在一起、60s 限流形同虚设（日志里能看到）。 */
                link_try_t0[0] = HAL_GetTick();
                UART_Printf("[WiFi] link0(PC) absent -> board2 only%c%c", 13U, 10U);
            }
            else if (step_idx == AT_STEP_CIPSTART1)
            {
                lt_note_link_down(1U);      /* ★ linktest：记一笔断开 */
                link_ok[1] = 0U;            /* ★ 板2 不在：记住，别再去踩 link1 */
                link_try_t0[1] = HAL_GetTick();     /* 同上：给 link1 也打时间戳 */
                UART_Printf("[WiFi] link1(board2) absent -> PC only%c%c", 13U, 10U);
            }
            /* ★ 必须清掉！可选步的失败回显里有 "0,CLOSED"/"1,CLOSED"，
               留着的话 1 秒后 READY 会把它当成"链路刚断"，
               平白触发一次限流补连（白折腾）。 */
            ESP_ClearRx();
        }
        if (r == 1U)                        /* 这一步真 OK 了：更新链路状态 */
        {
            if (step_idx == AT_STEP_CIPSTART0)      { if (link_ok[0] == 0U) { lt_note_link_up(0U); } link_ok[0] = 1U; }
            else if (step_idx == AT_STEP_CIPSTART1) { if (link_ok[1] == 0U) { lt_note_link_up(1U); } link_ok[1] = 1U; }
        }
        at_try = 0U;

        /* ★★ 补连单发：这一步（CIPSTART）处理完就回 READY，**不再往下走表**。
           否则补 link0 会顺带重发 link1 的 CIPSTART（把好链搞断）。
           注意 r==2（可选步失败）时上面已经把 link_ok 清掉、也清了缓冲，
           这里直接回 READY 即可。 */
        if (link_oneshot != 0U)
        {
            link_oneshot = 0U;
            wifi_state   = WIFI_ST_READY;
            ready_t0     = HAL_GetTick();   /* 重开 1 秒冷静期，别立刻又判断开 */
            ESP_ClearRx();
            /* ★★ 实验六修正：补连 = 一条新 TCP 流，接收字节流一定不连续。
               主流程走完 AT 表进 READY 时是有 px_reset() 的，link1 断链处也有，
               唯独这条补连路径原先漏了 —— px_feed() 的 "+IPD" 状态机若残留
               半帧（比如旧链路上刚收到 "+IPD,0,2" 就断了），新链路的第一帧
               会被当成旧帧的续体吃掉。补上，与另外两处保持一致。 */
            px_reset();
            break;
        }

        step_idx++;
        if (step_idx >= (u8)AT_COUNT)
        {
            wifi_err    = 0U;
            retry_count = 0U;               /* 连上了，连续失败计数清零 */
            pwr_off_tmo = ESP_TMO_PWR_OFF;
            wifi_state  = WIFI_ST_READY;
            ready_t0    = HAL_GetTick();    /* 冷静期起点：见 READY 分支 */
            /* ★ 把连接阶段的残留回显倒掉（CWJAP 的 "WIFI DISCONNECT"、
               连失败留下的 "1,CLOSED"…）。不清的话 1 秒冷静期是"假冷静"——
               冷静期一过就撞上这些老字节，误判掉线 -> 开机就整模块重连一次
               （实测丢了 ~33 帧 / 16 秒）。 */
            ESP_ClearRx();
            px_reset();                     /* ★ v8：刚重连上，帧流从干净状态开始找 */
            UART_Printf("%c%c[WiFi] ==== LINK UP : link0=PC %s, link1=board2 %s ====%c%c",
                        13U, 10U,
                        (link_ok[0] != 0U) ? "OK" : "absent",
                        (link_ok[1] != 0U) ? "OK" : "absent",
                        13U, 10U);

            /* ★★ linktest 修复 v2：**不再补发 ATE0**（v1 的方案被日志否证）。
               第五轮日志证明：这块 ESP-01 固件根本不吃 ATE0 —— 补发的 ATE0
               照样被回显（`>> ATE0` 后紧跟 `<< ATE0`），echo 计数继续涨。
               而且 v1 在 READY 里裸发 ATE0、不等回应就发数据帧，反而把
               "ATE0 回显"堵进接收缓冲，是 `Recv 21 bytes` 的帮凶。
               改为：**软件侧彻底忽略回显行**（见 ESP_Collect 的 StripEcho），
               完全不依赖模块配合。这里只留一个 1 秒"发送刹车"（详见 READY
               分支的 tx_hold_until），让模块把 CONNECT 收尾消化完。*/
        }
        else
        {
            ESP_Begin(at_cmd[step_idx], (u32)at_tmo[step_idx]);
        }
        break;

    /* ---- 链路就绪：收帧 + 从发送队列取一帧发出去 ---- */
    case WIFI_ST_READY:
        ESP_Collect();

        /* 上位机/板2 被关掉、热点断了，模块会主动吐这些词。
           ★ 但刚进 READY 的 1 秒内不判：连接过程中的残留回显
           （CWJAP 的 "WIFI DISCONNECT"、连板2 失败留下的字）会**误触发**，
           导致"刚连上就重连"、白折腾十几秒。*/
        if ((HAL_GetTick() - ready_t0) >= 1000U)
        {
            if (ESP_Find("WIFI DISCONNECT") != 0U)
            {
                why_wifilost++;
                UART_Printf("[WiFi] WiFi lost -> full reconnect%c%c", 13U, 10U);
                Wifi_Retry();               /* WiFi 断了：整模块重来（该重连路由）*/
                break;
            }
            /* ★ link0(PC) 断了 —— **只标记 + 限流补连，不整模块重连**。
               走的还是老规矩：不调 Wifi_RetryTcp()（那会清空发送队列、
               重走 CIPMUX，把还活着的链路一起拆掉）。
               ★ 实验六：PC 是**唯一**的上报链路，这条就是主通路 ——
                 PC 上位机重启/被关掉时，模块通常会吐 `0,CLOSED`，
                 靠这里 5 秒内把它挂回来。 */
            if (ESP_Find("0,CLOSED") != 0U)
            {
                why_closed++;
                lt_note_link_down(0U);      /* ★ linktest：记一笔断开 */
                link_ok[0] = 0U;
                ESP_ClearRx();
                UART_Printf("[WiFi] link0(PC) closed -> try re-link%c%c", 13U, 10U);
                (void)Wifi_TryLink(0U);
                break;
            }
            /* ★ 实验5 新增：板2(link1) 断了 —— 标记掉 + 限流补连。
               （不动 link0/PC：那是另一条链路，PC 这边好好的。）
               ★ 诊断：板2 的连接如果被对端/模块关掉，把**原始字节**打出来，
                 分清是"真的 1,CLOSED"还是"帧数据里恰好含这个词"（误判）。
                 打 hex 而不是字符串，避免里面的 CR/LF 把日志搅乱。
               ★★ 实验六：没有 link1 了，这段整块编译掉 —— 它一旦误命中
                 （比如某帧数据里恰好出现 "1,CLOSED"），就会 `break`
                 跳过发送闸门，等于平白停发一拍。 */
#if WIFI_HAS_LINK1
            if (ESP_FindLinkClosed(1U) != 0U)
            {
                ESP_ShowHex("<< link1 closed raw:");
                lt_note_link_down(1U);      /* ★ linktest：记一笔断开 */
                link_ok[1] = 0U;
                ESP_ClearRx();
                px_reset();                 /* ★ v8：流断了，别让状态机卡在半帧里 */
                UART_Printf("[WiFi] link1(board2) closed -> try re-link%c%c", 13U, 10U);
                (void)Wifi_TryLink(1U);
                break;
            }
#endif
        }

        /* ★★ 周期性补连"缺失的链路"（板 → PC）。
           为什么需要它：link0 的 CIPSTART 是**可选步**（at_opt[6]=1），
           开机时 PC 上位机通常还没跑，那一步会被拒/超时并跳过 ——
           之后若没人管，这条链路就永远挂不上（"连不上就不发，不发就永远
           不知道对方上线了"）。所以 READY 里必须周期性地把它挂回来。

           ★ 为什么这个间隔可以取短（= LINK_RETRY_MS 5s）：
             本分支只在 `link_ok[0] == 0` **且**已过 1 秒冷静期时才进 ——
             此时链路本来就不通、Wifi_Push 本来就在丢帧，不存在"打扰数据流"，
             越早补上越好。（实验五那个 60s 是为"别打扰板1↔板2 主链路"设的，
             实验六板间走 CAN，已无此约束，见 PC_RETRY_MS 定义处。）
           ★ 一次只补一条；`did_try` 只在**真正发起**补连时才为 1，
             因为 Wifi_TryLink 返回 0 有两种含义（"已发起"和"暂时不能发"），
             混用会像 1652348 那样把后面的发送闸门饿死。 */
        if ((HAL_GetTick() - ready_t0) >= 1000U)
        {
            u8 did_try = 0U;

#if WIFI_HAS_LINK1
            if (link_ok[1] == 0U)
            {
                did_try = (u8)((Wifi_TryLink(1U) == 0U) ? 1U : 0U);
            }
            else
#endif
            if ((link_ok[0] == 0U) &&
                ((HAL_GetTick() - link_try_t0[0]) >= PC_RETRY_MS))
            {
                did_try = (u8)((Wifi_TryLink(0U) == 0U) ? 1U : 0U);
            }

            if (did_try != 0U)
            {
                break;                      /* 已发起补连：本拍到此为止 */
            }
        }

        /* ★ v8：帧不在这里解析了 —— px_feed() 已在 UART 接收中断里逐字节取走
           （状态跨拍保留，"帧头被切开"也无害）。本拍只维护"AT 关键字行视图"
           的边界：看一眼、清掉这一段。**清空再也吞不掉帧**（F5/M1 同时消失）。*/
        ESP_ClearRx();

        /* 两次发送之间留间隔：连着催，模块会回 "busy s..." 然后挂掉。
           间隔是可变的（tx_gap_ms）：模块一回 busy 就拉长，缓过来再缩回去。
           ★★ linktest 修复 v2：**刚进 READY 的 1 秒内不发数据帧**。
           第五轮日志铁证：`1,CONNECT\r\n\r\nOK` 之后我们 0 毫秒就发 CIPSEND，
           模块内部还在处理 CIPSTART 收尾、根本没进接收态，把数据当命令吃掉
           → `Recv 21 bytes`（21 = 握手帧长）。连撞 3 次就整模块断电重连，
           于是 up1 只有 12%、down1 动辄 5~18 秒。
           刹车做法：把 ready_t0 当作发送闸门（见下方条件里的 tx_hold）。 */
        if ((tx_pending == 0U) && ((HAL_GetTick() - tx_done_t0) >= tx_gap_ms) &&
            ((HAL_GetTick() - ready_t0) >= 1000U))
        {
            u8 k;

            if (urg_pending != 0U)              /* 优先槽：控制帧插队先发 */
            {
                tx_link = urg_link;
                tx_len  = urg_len;
                for (k = 0U; k < tx_len; k++) { tx_buf[k] = urg_buf[k]; }
                urg_pending = 0U;
            }
            else if (txq_cnt != 0U)             /* 普通队列 */
            {
                tx_link = txq_link[txq_head];
                tx_len  = txq_len[txq_head];
                for (k = 0U; k < tx_len; k++) { tx_buf[k] = txq_data[txq_head][k]; }
                txq_head = (u8)((txq_head + 1U) % TXQ_N);
                txq_cnt--;
            }
            else
            {
                break;                          /* 没东西可发 */
            }

            tx_pending = 1U;
            ESP_BeginCipSend(tx_link, tx_len);
            step_t0    = HAL_GetTick();
            step_tmo   = ESP_TMO_PROMPT;
            wifi_state = WIFI_ST_TX_PROMPT;
        }
        break;

    /* ---- 等模块回 ">" 提示符 ---- */
    case WIFI_ST_TX_PROMPT:
        ESP_Collect();
        if (ESP_FindPrompt() != 0U)
        {
            /* ★ 顺序：**先写数据，再清接收缓冲**（不能颠倒）。
               模块发 ">" 之后，只有**收到数据**才会去转发。
               ★★ v5：把数据帧也设为"要剔除的回显内容"——模块会把我们
               写的数据原样吐回，不剔掉就会污染 esp_line（见 ESP_SetEchoCmd）。*/
            ESP_SetEchoCmd(tx_buf, tx_len);
            ESP_SendRaw(tx_buf, tx_len);        /* 真正把数据发出去 */
            Wifi_DbgNoteCmd(tx_buf, tx_len);    /* ★ 回显探测：数据帧也记下 */
            ESP_ClearRx();
            step_t0    = HAL_GetTick();
            step_tmo   = ESP_TMO_SEND;
            wifi_state = WIFI_ST_TX_ACK;
        }
        else if ((ESP_Find("ERROR") != 0U) ||
                 (ESP_Find("busy s") != 0U) ||
                 ((HAL_GetTick() - step_t0) >= step_tmo))
        {
            u8 busy;
            u8 dead;
            ESP_ShowLine("<< CIPSEND failed:");
            /* ★★ linktest 修复 v3/v4：等 ">" 期间只认 ERROR / "busy s..." / 超时。
               （v4 起不再把 `Recv ` 当失败 —— 它是 SEND OK 的前导行，见 TX_ACK 注释。）
               这类失败统一当"模块还没准备好"：退避重试，不计 tx_fail、不清链路。*/
            busy = ((ESP_Find("busy s") != 0U) || (ESP_Find("Recv ") != 0U)) ? 1U : 0U;
            /* 链路真的没了（不是偶发超时）——立刻重连，别在死链上反复重试 */
            dead = ((ESP_FindLinkClosed(tx_link) != 0U) ||
                    (ESP_Find("link is not valid") != 0U) ||
                    (ESP_Find("SEND FAIL") != 0U)) ? 1U : 0U;

            tx_pending = 0U;
            tx_done_t0 = HAL_GetTick();
            if (busy != 0U)
            {
                /* ★ 模块没准备好（busy / Recv）：**丢掉这一帧**，退避越拉越长，
                   但**不清链路、不重连**。丢帧代价是上位机少几行，换来模块缓过来。*/
                busy_n++;
                tx_busy_cnt++;              /* ★ linktest */
                tx_gap_ms = (busy_n >= 3U) ? 400U : ((busy_n >= 2U) ? 300U : 250U);   /* ★ v11：退避上限 1000→400ms（别自己把吞吐压到 1/s）*/
                ESP_ClearRx();
                wifi_state = WIFI_ST_READY; /* 不算失败，歇一下再发 */
            }
            else
            {
                tx_err_cnt++;               /* ★ linktest：发送真失败 */
                tx_fail++;
                if (dead != 0U)
                {
                    /* ★ 实验5：哪条链路发挂了就只标记哪条 + 限流补连，
                       **不整模块重连**（另一条可能好好的）。 */
                    tx_fail = 0U;
                    if (tx_link <= 1U)
                    {
                        why_txdead++;
                        link_ok[tx_link] = 0U;
                        lt_note_link_down(tx_link);   /* ★ linktest：记录断开 */
                        UART_Printf("[WiFi] link%u dead -> try re-link%c%c",
                                    (u32)tx_link, 13U, 10U);
                        (void)Wifi_TryLink(tx_link);
                    }
                    ESP_ClearRx();
                    wifi_state = WIFI_ST_READY;
                }
                else if (tx_fail >= 3U)
                {
                    /* ★★ linktest 修复 v3：连撞 3 次**先别断电重连**，退回 READY。
                       理由：`Recv ` 已被上头归入"没准备好"（不计 tx_fail），
                       走到这里说明是 ERROR/超时。模块可能只是瞬时卡住，
                       再给它一个退避窗口比"断电重启几十秒"划算得多。
                       （真正的死链由 dead 分支处理；这里只降级为延长退避。）*/
                    tx_fail = 0U;
                    busy_n++;
                    tx_gap_ms = 1000U;          /* 拉长间隔，缓一缓 */
                    ESP_ClearRx();
                    wifi_state = WIFI_ST_READY;
                }
                else
                {
                    ESP_ClearRx();
                    wifi_state = WIFI_ST_READY; /* 偶发超时先回 READY，别断电 */
                }
            }
        }
        break;

    /* ---- 等 "SEND OK" ---- */
    case WIFI_ST_TX_ACK:
        ESP_Collect();
        if (ESP_Find("SEND OK") != 0U)
        {
            tx_pending = 0U;
            tx_fail    = 0U;                /* 发出去了，失败计数清零 */
            busy_n     = 0U;                /* 发顺了：退避计数与间隔都缩回去 */
            tx_ok_cnt++;                    /* ★ linktest：真发出去一帧 */
            tx_gap_ms  = ESP_TMO_GAP;
            tx_done_t0 = HAL_GetTick();
            ESP_ClearRx();
            wifi_state = WIFI_ST_READY;
        }
        /* ★★★ 关键修复 v4（真凶）：**`Recv N bytes` 不是错误，是 `SEND OK` 的前导行！**
           ESP8266 正常发数据的回显顺序是：
               >              (提示符)
               Recv 8 bytes   (模块确认"我收到了 8 字节" ← 先来)
               SEND OK        (转发成功 ← 后到)
           老代码一看到 `Recv ` 就判失败并跳出 —— 正好卡在这两行**之间**，
           永远等不到紧随其后的 `SEND OK`，于是 xok 恒为 0！
           这就是"数据其实发出去了、板2 也收到了，但我们自己判失败"的真相
           （日志铁证：板2 rx=3 与自身 xerr=3 恒等；板1 ack 一路涨到 60%）。
           现在改为：`Recv ` 只当**中间态**，继续等 `SEND OK`。
           只有真正的错误（ERROR / busy s...）或超时才判失败。*/
        else if ((ESP_Find("ERROR") != 0U) ||
                 (ESP_Find("busy s") != 0U) ||      /* "busy s..." = 发送忙 */
                 ((HAL_GetTick() - step_t0) >= step_tmo))
        {
            u8 busy;
            u8 dead;
            ESP_ShowLine("<< SEND failed:");
            /* ★★ linktest 修复 v3：同 TX_PROMPT —— busy/Recv 统一当"模块没准备好"，
               走退避、**不累加 tx_fail、不重连**。这是"3 次撞死就断电"死循环的正解。*/
            busy = ((ESP_Find("busy") != 0U) || (ESP_Find("Recv ") != 0U)) ? 1U : 0U;
            /* ★ "Recv N bytes" = 模块没进接收态就把我们写的数据当成命令收了。
               这一帧**没发出去**，但**不是链路的错**，按"没准备好"退避重试。*/
            if (ESP_Find("Recv ") != 0U)
            {
                tx_err_cnt++;                   /* ★ linktest：单独记数（观测用）*/
            }
            dead = ((ESP_FindLinkClosed(tx_link) != 0U) ||
                    (ESP_Find("link is not valid") != 0U) ||
                    (ESP_Find("SEND FAIL") != 0U)) ? 1U : 0U;

            tx_pending = 0U;
            tx_done_t0 = HAL_GetTick();
            if (busy != 0U)
            {
                /* 同上：丢帧 + 递增退避，不清链路（见 TX_PROMPT 里的说明） */
                busy_n++;
                tx_gap_ms = (busy_n >= 3U) ? 400U : ((busy_n >= 2U) ? 300U : 250U);   /* ★ v11：退避上限 1000→400ms（别自己把吞吐压到 1/s）*/
                ESP_ClearRx();
                wifi_state = WIFI_ST_READY; /* 模块没准备好：不算失败 */
            }
            else
            {
                tx_fail++;
                if (dead != 0U)
                {
                    /* ★ 同 TX_PROMPT：只标记挂掉的那条 + 限流补连 */
                    tx_fail = 0U;
                    if (tx_link <= 1U)
                    {
                        lt_note_link_down(tx_link);   /* ★ linktest：记录断开 */
                        link_ok[tx_link] = 0U;
                        UART_Printf("[WiFi] link%u dead -> try re-link%c%c",
                                    (u32)tx_link, 13U, 10U);
                        (void)Wifi_TryLink(tx_link);
                    }
                    ESP_ClearRx();
                    wifi_state = WIFI_ST_READY;
                }
                else if (tx_fail >= 3U)
                {
                    /* ★★ linktest 修复 v3：连撞 3 次**先别断电重连**，退回 READY。
                       理由：`Recv ` 已被上头归入"没准备好"（不计 tx_fail），
                       走到这里说明是 ERROR/超时。模块可能只是瞬时卡住，
                       再给它一个退避窗口比"断电重启几十秒"划算得多。
                       （真正的死链由 dead 分支处理；这里只降级为延长退避。）*/
                    tx_fail = 0U;
                    busy_n++;
                    tx_gap_ms = 1000U;          /* 拉长间隔，缓一缓 */
                    ESP_ClearRx();
                    wifi_state = WIFI_ST_READY;
                }
                else
                {
                    ESP_ClearRx();
                    wifi_state = WIFI_ST_READY; /* 单次失败先回 READY */
                }
            }
        }
        break;

    /* ---- 歇 3 秒，然后给模块断电重启，从头再来 ---- */
    case WIFI_ST_RETRY:
        if ((HAL_GetTick() - step_t0) >= ESP_TMO_RETRY)
        {
            ESP_EnWrite(ESP_EN_OFF);
            step_t0    = HAL_GetTick();
            wifi_state = WIFI_ST_PWR_LOW;
        }
        break;

    default:
        break;
    }
}

/* 链路在不在：READY 和两个发送子状态都算"连着"。
   （发送过程中状态会短暂离开 READY，不算掉线，否则 LCD 会一闪一闪）*/
u8 Wifi_IsReady(void)
{
    /* ★ 实验5：两条链路都是可选的，只要**任一条**连着就算"就绪"。
       典型场景：PC 上位机没开，只有板1↔板2 直连 —— 这时 LCD/采样照常跑，
       绝不能让"PC 不在"把整个应用判成离线。
       发送过程中状态短暂离开 READY 也不算掉线，LCD 不会闪。 */
    if ((link_ok[0] != 0U) || (link_ok[1] != 0U))
    {
        return 1U;
    }
    if ((wifi_state == WIFI_ST_READY) ||
        (wifi_state == WIFI_ST_TX_PROMPT) ||
        (wifi_state == WIFI_ST_TX_ACK))
    {
        return 1U;
    }
    return 0U;
}

/* ==== 实验5：查某条链路真连上了没（供 app 层判断握手/显示）==== */
u8 Wifi_LinkOk(u8 slot)
{
    if (slot > 1U)
    {
        return 0U;
    }
    /* ★ linktest：顺手维护在线时长底账 —— 这里是被反复调用的"观测点"。
       每 5ms 被 app 的 lt_tick 调一次，用调用间隔累计在线毫秒。
       （不用 HAL_GetTick 直接算差分，避免同一 ms 内多次调用多记。）*/
    {
        u32 now = HAL_GetTick();
        u32 dt  = now - link_ok_t0;
        if (link_ok_t0 != 0U)
        {
            if (link_ok[0] != 0U) { link_up_ms[0] += dt; }
            if (link_ok[1] != 0U) { link_up_ms[1] += dt; }
        }
        link_ok_t0 = now;
    }
    return link_ok[slot];
}

/*==============================================================================
 *  ★★★ linktest：底账透视接口（给 app 层做压测统计用）★★★
 *
 *  为什么要开这个口子？
 *    app 层看到的 link_ok 只是"快照"。链路到底是"真的断"还是"被误判成断"，
 *    必须看 WiFi 层内部的底账。现在全部摊开：
 *      - link_ok_raw[]  : CIPSTART 成功(1) / 报错或 SEND 失败(0) 的原始标志
 *      - down_cnt[]     : link_ok 由 1 翻 0 的**次数**（就是"断了几次"）
 *      - tx_ok / tx_busy / tx_err : 发送三态各发生多少次
 *      - rx_ipd         : 成功抠出 +IPD 数据的次数（对方真发过来了几次）
 *      - rx_bytes       : UART4 累计实收字节（模块在不在说话）
 *      - cmd_cnt / cmd_bytes : 发过多少条 AT 命令、共多少字节
 *    有这组数，"断没断"就是算术题，不用再靠肉眼看串口刷屏。
 *============================================================================*/
u8 Wifi_DbgLinkRaw(u8 slot)
{
    return (slot < 2U) ? link_ok[slot] : 0U;
}

u32 Wifi_DbgUptimeMs(u8 slot)
{
    return (slot < 2U) ? link_up_ms[slot] : 0U;
}

u32 Wifi_DbgDownCnt(u8 slot)
{
    return (slot < 2U) ? link_down_cnt[slot] : 0U;
}

u32 Wifi_DbgDownMaxMs(u8 slot)
{
    return (slot < 2U) ? link_down_max[slot] : 0U;
}

u32 Wifi_DbgTxOk(void)   { return tx_ok_cnt; }
u32 Wifi_DbgTxBusy(void) { return tx_busy_cnt; }
u32 Wifi_DbgTxErr(void)  { return tx_err_cnt; }
u32 Wifi_DbgRxIpd(void)  { return rx_ipd_cnt; }
u32 Wifi_DbgRxBytes(void){ return rx_bytes_cnt; }
u32 Wifi_DbgCmdCnt(void) { return cmd_cnt; }
u32 Wifi_DbgCmdBytes(void){ return cmd_bytes_cnt; }
u32 Wifi_DbgQHi(void)     { return (u32)txq_hi; }

/* 取走队列高水位并复位（每次上报后调一次）*/
u32 Wifi_DbgQHiTake(void)
{
    u32 v = (u32)txq_hi;
    txq_hi = 0U;
    return v;
}
u32 Wifi_DbgBogusPrompt(void) { return lt_prompt_bogus; }

/* ★ v7：诊断计数透视（id 见 esp01s.h 的 V7_*）*/
u32 Wifi_DbgV7(u8 id)
{
    switch (id)
    {
        case V7_CUT:     return v7_cut;
        case V7_CLR_IPD: return v7_clr_ipd;
        case V7_CLR_X:   return v7_clr_x;
        case V7_BATCH:   return v7_batch;
        case V7_OVR:     return v7_ovr;
        case V7_STRIP:   return v7_strip;
        case V7_RESID_H: return v7_resid_h;
        case V7_RESID_T: return v7_resid_t;
        case V7_HALF:    return v7_half;
        case V7_OVF:     return v7_ovf;
        case V7_ORE:     return v7_ore;
        case V7_LFULL:   return v7_lfull;
        case V7_ORE_C0:  return v7_ore_code[0];
        case V7_ORE_C1:  return v7_ore_code[1];
        case V7_ORE_C2:  return v7_ore_code[2];
        default:         return 0U;
    }
}

/*==============================================================================
 *  ★★★ linktest：回显探测（判断 ATE0 到底有没有生效）★★★
 *
 *  思路：把**最近一次发出去的原始字节**留一份，然后在收到的数据里找它。
 *    模块若开着回显（ATE0 失效），我们发的每个字节都会被**原样吐回**，
 *    于是 esp_line 里必然出现"刚发过的那串命令"。
 *    这在日志里表现为：`>> AT+CIPSTART=1,...` 之后
 *    收到的内容里也有一模一样的 `AT+CIPSTART=1,...`。
 *
 *  echo_seen 累计"回显命中次数"；echo_last_len 记最近命中的长度。
 *  若 echo_seen 持续增长 → ATE0 没生效，一切判据都被回显污染。
 *============================================================================*/
static u8  echo_probe[40];              /* 最近一条命令的前若干字节 */
static u16 echo_probe_len;
static u32 echo_seen;                   /* 回显命中次数 */

void Wifi_DbgNoteCmd(const u8 *buf, u16 len)
{
    u16 n = (len > (u16)sizeof(echo_probe)) ? (u16)sizeof(echo_probe) : len;
    u16 i;

    for (i = 0U; i < n; i++) { echo_probe[i] = buf[i]; }
    echo_probe_len = n;
}

/* 在收到的 esp_line 里找"刚发过的命令"—— 找到就是回显 */
static void lt_check_echo(void)
{
    u16 i;
    u16 j;

    if ((echo_probe_len == 0U) || (esp_len < echo_probe_len))
    {
        return;
    }
    for (i = 0U; (u16)(i + echo_probe_len) <= esp_len; i++)
    {
        for (j = 0U; j < echo_probe_len; j++)
        {
            if (esp_line[i + j] != echo_probe[j]) { break; }
        }
        if (j == echo_probe_len)
        {
            echo_seen++;
            return;
        }
    }
}

u32 Wifi_DbgEchoSeen(void) { return echo_seen; }

/*------------------------------------------------------------------------------
 *  投递一帧数据到发送队列（非阻塞）。
 *      link：0 = PC，1 = 板2（CIPMUX=1 的 link id）
 *      返回 1 = 收下了；0 = 队列满 / 没连上 / 长度非法（直接丢）
 *  同一帧要发两条链接时，调用者 push 两次即可（队列 4 深，够用）。
 *----------------------------------------------------------------------------*/
u8 Wifi_Push(const u8 *buf, u16 len, u8 link)
{
    u8 k;

    /* ★ 能不能发只看 link_ok（它已经涵盖"整模块正在重连"）——
       不再看状态机当前在哪个状态，这样"补连 link1"期间发 PC 的帧照样能排队。 */
    if (link > 1U)
    {
        return 0U;
    }
    if (link_ok[link] == 0U)
    {
        return 0U;                          /* 这条链路没连上：直接丢，别去踩 CIPSEND */
    }
    if ((len == 0U) || (len > ESP_TX_SIZE) || (txq_cnt >= TXQ_N))
    {
        return 0U;
    }

    txq_link[txq_tail] = link;
    txq_len[txq_tail]  = len;
    for (k = 0U; k < len; k++)
    {
        txq_data[txq_tail][k] = buf[k];
    }
    txq_tail = (u8)((txq_tail + 1U) % TXQ_N);
    txq_cnt++;
    if (txq_cnt > txq_hi) { txq_hi = (u8)txq_cnt; }   /* ★ linktest：高水位 */
    return 1U;
}

/*------------------------------------------------------------------------------
 *  投递一帧"优先"数据（控制帧专用）：写进优先槽，READY 时插队先发。
 *  同一时刻只留最后一条 —— 连按按键时，灯只认最终状态。
 *----------------------------------------------------------------------------*/
u8 Wifi_PushUrgent(const u8 *buf, u16 len, u8 link)
{
    u8 k;

    if (link > 1U)
    {
        return 0U;
    }
    if (link_ok[link] == 0U)
    {
        /* ★ 控制帧是"按键"触发的、用户正等着：顺手把这条链路补连上。
           本次返回 0，app 会把 ctrl_pending 登记回去、下一拍重投 ——
           所以"板2 后上电，按一次键"就能自愈。 */
        (void)Wifi_TryLink(link);
        return 0U;
    }
    if ((len == 0U) || (len > ESP_TX_SIZE))
    {
        return 0U;
    }

    urg_link = link;
    urg_len  = len;
    for (k = 0U; k < len; k++)
    {
        urg_buf[k] = buf[k];
    }
    urg_pending = 1U;
    return 1U;
}

/* 给 LCD / 串口看的状态名 */
const char *Wifi_StateName(void)
{
    switch (wifi_state)
    {
    case WIFI_ST_OFF:      return "OFF";
    case WIFI_ST_PWR_LOW:  return "POWER";
    case WIFI_ST_PWR_WAIT: return "BOOT";
    case WIFI_ST_CMD:      return "CONNECT";
    case WIFI_ST_READY:    return "READY";
    case WIFI_ST_TX_PROMPT:
    case WIFI_ST_TX_ACK:   return "SENDING";
    case WIFI_ST_RETRY:    return "RETRY";
    default:               return "?";
    }
}
