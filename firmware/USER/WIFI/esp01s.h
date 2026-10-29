/*==============================================================================
 *  esp01s.h  ---  ESP8266-01（蓝色 8 脚小板，不是 6 脚的 01S）AT 驱动，挂 UART4
 *
 *  ★ ESP-01S 走 **UART4（PA0/PA1）**（原先走 USART6 PC6/PC7，实测 PA0/PA1 更稳，已改）。
 *    硬件接法同前，只是两根数据线从 PC6/PC7 挪到 PA0/PA1。
 *
 *  ESP-01 有 8 个脚，比 01S 多出 CH_PD 和 RST，飞线时最容易漏掉 CH_PD ——
 *  漏了模块根本不启动，现象就是"一个字节都收不到"。
 *
 *  引脚（对着排针看，两端是 GND / VCC，具体以模块丝印为准）：
 *      1 GND       5 TXD
 *      2 GPIO2     6 CH_PD (EN)
 *      3 GPIO0     7 RST
 *      4 RXD       8 VCC
 *
 *  ---- 必须接（少一个就完全不说话）----
 *      VCC      ->  3.3V   （发射瞬间要 300mA，别从板上弱的 3.3V 上抢）
 *      GND      ->  GND    （必须和单片机共地）
 *      CH_PD/EN ->  3.3V   （不接 = 模块永远不启动，飞线第一大坑）
 *      ▸ 注意：本板（启明欣欣）把模块的 CH_PD 接到了 MCU 的 **PF14**，
 *        不是直接接 3.3V。所以必须由程序把 PF14 拉高，模块才工作 ——
 *        见 ESP_PowerOn()。用 USB-TTL 单独测模块时，才需要手动把 CH_PD 接 3.3V。
 *      TXD      ->  PA1    (UART4_RX)
 *      RXD      ->  PA0    (UART4_TX)
 *
 *  ---- 可以不接（板上已有上拉/RC），但千万别拉低 ----
 *      RST      （拉低会一直复位；悬空一般就行，也可 10k 上拉到 3.3V）
 *      GPIO0    （拉低 = 进串口下载模式，永远不跑 AT）
 *      GPIO2    （拉低启动会失败）
 *
 *  ---- 电源 ----
 *      模块 VCC 和 GND 之间并 100uF 电解 + 0.1uF 瓷片，越靠近模块越好。
 *      电压要在模块自己的排针上量，不要在开发板排针上量（线损能差 0.3V）。
 *
 *  ---- 波特率 ----
 *      usart.c 里 UART4 = 115200，串口助手也是 115200，两者必须一致。
 *      模块上电会在 74880bps 先吐一段开机日志（在 115200 下看是乱码，正常），
 *      之后才在 115200 上说 AT 的话。若整段完全没有反应，把 usart.c 里那行
 *      改成 9600 重编一次（个别老模块是 9600）。
 *
 *  ---- 注意 ----
 *      如果模块被刷过别的固件（NodeMCU / Arduino / MicroPython），
 *      它不会认 AT 指令，只会乱打或干脆沉默，那不是接线问题。
 *============================================================================*/
#ifndef __ESP01S_H
#define __ESP01S_H

#include "common.h"
#include "app_id.h"     /* 取 STU_WIFI_SSID / STU_WIFI_PASS（换人只改 app_id.h 的 STU_WHO）*/

/* ================== 下面 2 项按你的实际情况改 ==================
   WiFi 名/密码已挪到 app_id.h 的 STU_WHO 分档里，这里只是转一下。*/
#define ESP_WIFI_SSID   STU_WIFI_SSID           /* 路由器 WiFi 名称（在 app_id.h）*/
#define ESP_WIFI_PASS   STU_WIFI_PASS           /* 路由器 WiFi 密码（在 app_id.h）*/
#define ESP_TCP_IP      "192.168.137.1"         /* 电脑「移动热点」的固定网关地址 */
#define ESP_TCP_PORT    8080                    /* 上位机监听端口（无 U 后缀，数字要拼进 AT 指令）*/

/*==============================================================================
 *  角色相关的地址 —— 同一个工程、三个 Target，靠 BOARD_SEL 区分
 *
 *  ★ 实验六：三块板都是 **TCP 客户端**，各自主动连 PC:8080；板间数据走 CAN。
 *    每块板钉死自己的静态 IP（避免 DHCP 冲突，也便于排错）：
 *        板1 -> .51   板2 -> .50   （实验七只有两块板）
 *============================================================================*/
#if (BOARD_SEL == 2)
#define ESP_STATIC_IP   "192.168.137.50"
#elif (BOARD_SEL == 3)
#define ESP_STATIC_IP   "192.168.137.52"
#else
#define ESP_STATIC_IP   "192.168.137.51"
#endif

/* 实验五遗留的"对板"链路（link1 → 192.168.137.50:8081）：实验六板间走 CAN，
   已不再使用。宏保留只是为了不改动 esp01s_client.c 的 AT 流程表
   （那一步是**可选步**，连不上就跳过，不影响到 PC 的主链路）。 */
#define ESP_PEER_IP     "192.168.137.50"
#define ESP_PEER_PORT   8081
#define ESP_SERVER_PORT 8081

/*==============================================================================
 *  联网状态机（全部非阻塞，靠 WifiTask 里的 Wifi_Tick() 一步步推进）
 *
 *  旧版是一个 ESP_WifiConnect() 一口气跑完（CWJAP 一等就是 20 秒），
 *  在它返回之前主循环一步都动不了 —— 按键、采集、LCD 全冻住。
 *  新版把它拆成下面这些状态，每次 Tick 只推进一步就返回。
 *
 *  流程：POWER(断电300ms) -> BOOT(等2s) -> CMD(AT -> ATE0 -> CWMODE=1
 *        -> CWJAP -> CIPMUX=0 -> CIPSTART) -> READY
 *  任何一步失败 -> RETRY(歇3s) -> 回到 POWER 重来
 *  第一条 AT 撞上 "busy p..."（模块刚上电还在忙）时，先原地重试 3 次，
 *  不用整个模块断电重来。
 *  连续失败 3 轮以上 -> 断电时间自动拉长到 5 秒（模块卡死时光拉 CH_PD
 *  300ms 是救不回来的），并提示人去拔一下板子的 USB。
 *============================================================================*/
#define WIFI_ST_OFF        0U   /* 还没开始 */
#define WIFI_ST_PWR_LOW    1U   /* 模块断电中 */
#define WIFI_ST_PWR_WAIT   2U   /* 等模块启动 */
#define WIFI_ST_CMD        3U   /* 正在走 AT 流程表 */
#define WIFI_ST_READY      4U   /* 已连上上位机，可以发数据 */
#define WIFI_ST_TX_PROMPT  5U   /* 等模块回 ">" */
#define WIFI_ST_TX_ACK     6U   /* 等 "SEND OK" */
#define WIFI_ST_RETRY      7U   /* 掉线，歇一会儿重连 */

extern u8 wifi_state;           /* 当前联网状态，见上面 WIFI_ST_* */
extern u8 wifi_err;             /* 失败时卡在流程表第几步（1..6），0 = 没出错 */

void Wifi_Start(void);          /* 开始后台联网（不阻塞，立刻返回）*/
void Wifi_Tick(void);           /* 主循环每次调一次，推进一步（不阻塞）*/
u8   Wifi_IsReady(void);        /* 1 = 已连上上位机 */
/* ★ 实验5：查某条链路是否真的连着。slot: 0=PC，1=对板。
   用途：LCD / 握手重发判断 —— 别拿"状态机在 READY"当成"链路通了"
   （两条 TCP 都是可选步，可能一条都没连上）。 */
u8   Wifi_LinkOk(u8 slot);

/* ★★★ linktest：底账透视接口（临时调试，验收后删）★★★
 * 把 WiFi 层内部计数摊给 app 层，让"链路到底断没断"变成算术题。 */
u8   Wifi_DbgLinkRaw(u8 slot);      /* 原始 link_ok 快照 1=连上 */
u32  Wifi_DbgUptimeMs(u8 slot);     /* 该链路累计在线毫秒 */
u32  Wifi_DbgDownCnt(u8 slot);      /* 该链路断开次数 */
u32  Wifi_DbgDownMaxMs(u8 slot);    /* 该链路最长一次断开毫秒 */
u32  Wifi_DbgTxOk(void);            /* SEND OK 次数（真发出去几帧）*/
u32  Wifi_DbgTxBusy(void);          /* 模块回 busy 次数 */
u32  Wifi_DbgTxErr(void);           /* 发送失败次数 */
u32  Wifi_DbgRxIpd(void);           /* 收到 +IPD 帧数 */
u32  Wifi_DbgRxBytes(void);         /* UART4 累计实收字节 */
u32  Wifi_DbgCmdCnt(void);          /* 发过多少条 AT 命令 */
u32  Wifi_DbgCmdBytes(void);        /* AT 命令累计字节 */
u32  Wifi_DbgQHi(void);             /* 发送队列高水位 */
u32  Wifi_DbgQHiTake(void);         /* 取走队列高水位并复位 */
u8   Wifi_DbgPeerLinkId(void);      /* 服务器：学到板1 的硬件 link id（0xFF=没学到）*/
u32  Wifi_DbgBogusPrompt(void);     /* 非行首 '>' 被拦下的次数（=0 说明修复有效）*/
u32  Wifi_DbgEchoSeen(void);        /* 收到的数据里命中"自己刚发的内容"的次数（>0 = ATE0 失效）*/

/* ★ 实验七新增：**重连原因计数**（排查"为什么老断"的第一手证据）
 *   id: 0 = 收到 0,CLOSED（对端关连接 / 上位机重启）
 *       1 = CIPSEND 失败且判死链（半开连接：对端静默消失）
 *       2 = WIFI DISCONNECT（热点掉了）
 *       3 = AT 流程某步失败（模块没答话 / 被复位）
 *       4 = 连续失败进深断电（模块真的不答话了 → 查 ESP 供电）
 *   串口敲 w 打印。 */
u32  Wifi_DbgWhy(u8 id);

/* ★★★ linktest v7：丢帧解剖计数（只加读数，不改行为；见《linktest诊断v7实施方案》）★★★
 * 前 12 个（V7_CUT~V7_LFULL）在 app 层做**窗口增量**打印；V7_ORE_C0~C2 为原始值直印。 */
#define V7_CUT      0U   /* 清理前 esp_line 行尾是 "+IPD," 的 1~4 字节真前缀（头被切）*/
#define V7_CLR_IPD  1U   /* pr==0 且行内含 "+IPD"（清空时行内确有帧痕迹）*/
#define V7_CLR_X    2U   /* 任意 ClearRx 时行内含 "+IPD"（补连/断链/CIPSEND 前，M2 观测）*/
#define V7_BATCH    3U   /* 单次解析抠出 ≥2 帧（单槽覆盖风险前置）*/
#define V7_OVR      4U   /* 单槽覆盖：旧帧还没被取走就写入新帧 */
#define V7_STRIP    5U   /* ESP_StripEcho 实际删掉的行数（验证剔除是否在干活）*/
#define V7_RESID_H  6U   /* 行首 6B 命中某条 echo 条目首 6B（半截回显，拍级）*/
#define V7_RESID_T  7U   /* 行尾 6B 命中某条 echo 条目尾 6B（半截回显，拍级）*/
#define V7_HALF     8U   /* stall≥20 拍保底清空触发次数 */
#define V7_OVF      9U   /* UART 中断里环形满而丢掉的字节数 */
#define V7_ORE      10U  /* UART4 错误回调次数（前 3 次记 errorcode）*/
#define V7_LFULL    11U  /* esp_line 见顶且环形仍有积压 */
#define V7_ORE_C0   12U  /* 第 1 次错误的 errorcode（原始值）*/
#define V7_ORE_C1   13U
#define V7_ORE_C2   14U
#define V7_N        12U  /* 前 12 个参与窗口增量；12~14 直印 */
u32  Wifi_DbgV7(u8 id);             /* v7 诊断计数透视 */

/* 取一条收到帧（从 +IPD 抠出，单槽：新的覆盖旧的）。
 *   ★ 实验5 起两块板都能收：板1 收板2、板2 收板1。
 *   out: 输出缓冲; max: 缓冲大小。返回 1 = 取到一条；0 = 暂时没有。 */
u8   Wifi_GetRx(u8 *out, u16 max);
/* 投递一帧（非阻塞）。slot: 0=PC，1=对板（两块板同义）。
 *   板1（客户端）：slot0 -> link0(PC)、slot1 -> link1(板2)，link 固定；
 *   板2（服务器）：slot0 -> 固定 link4（主动连 PC）；
 *                 slot1 -> 板1 连入的那条 link（id 从 +IPD 动态学习，
 *                          未学习到之前直接丢弃）。 */
u8   Wifi_Push(const u8 *buf, u16 len, u8 slot);
u8   Wifi_PushUrgent(const u8 *buf, u16 len, u8 slot);  /* 优先帧：插队先发（控制帧用）*/
const char *Wifi_StateName(void);        /* 状态名，给串口排错用 */

#endif
