#include "app.h"
#include "common.h"
#include "uart_debug.h"
#include "lcd.h"
#include "chinese16.h"
#include "chinese_lcd.h"     /* ★ 题3：姓名用 24x24 字模（LCD_DisplayChinese）*/
#include "sht30.h"
#include "sgp30.h"
#include "can_link.h"
#include "i2c.h"            /* hi2c1 / hi2c2 + MX_I2C1/I2C2_Init —— I2C 总线恢复要用 */
#include "led.h"
#include "beep.h"
#include "led_effect.h"
#include "key.h"
#include "app_text.h"
#include "app_id.h"
#include "esp01s.h"
#include "light.h"
#include "ai_model.h"       /* 题2：推理封装（含 AI_ENABLED 开关）*/
#include "ai_test10.h"      /* 题2：题面要求的「任取 10 条后续记录」（tools/train_nn.py 生成）*/

/* ---- FreeRTOS（原生 API；CubeMX 生成的是 CMSIS_V1 包装，两套都在，这里用原生的更直观）---- */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/*==============================================================================
 *  实验七 · 两块 STM32F407（★ 两板**等价**：同一份代码、同样的按键行为）
 *
 *  ★ 采集开关（题1）：**按任意一板的 KEY0/KEY1，两块板同时开始/停止**
 *      KEY0 → 本板 running=1，并经 CAN 通知对板一起开（板1 发 0x200 / 板2 发 0x201）
 *      KEY1 → 同上，一起停
 *      **上电两块板都不采**（实验六是"从机上电即采"；实验七改成对称，
 *      否则"按任意板 KEY0 同时开始"无从谈起）—— 见 App_Init。
 *
 *  两块板的**唯一差别**是各自负责的数据方向：
 *      板1（BOARD_SEL==1）：采集本板 5 项 → CAN 广播（板2 的「板1 区」靠它）
 *      板2（==2）         ：采集本板 5 项 → 收 CAN → 「板1 区」显示
 *  两块板**都**：WiFi 上报 PC（各自 TCP client → PC:8080）+ 呼吸灯 + 1Hz 心跳。
 *
 *  任务（两块板相同，7 个）：
 *      SensorTask   1s    读 SHT30 + 光敏 + SGP30 → g_state → 样本入队
 *      KeyTask      10ms  扫描按键（KEY0 开 / KEY1 停 / KEY2 切页 / KEY3 手动动作）
 *      CanTask      10ms  收发心跳与控制帧；板1 另广播数据，板2 另收数据
 *      WifiTask     5ms   推联网状态机 + 出队 → 组 D 帧 / S 帧上报 PC
 *      LedTask      20ms  推进 LED 效果（**两板都呼吸**，开关 = 本板 running）
 *      DisplayTask 200ms  读 g_state → 刷 LCD
 *      DebugTask    20ms  串口命令台（最低优先级）
 *
 *  规矩（踩过的坑）：
 *    a) 中文一律走 app_text.h 的 GB18030 字节数组（ARMCC 报 #870-D）；屏上字模走 chinese16.h
 *    b) 源码里不写反斜杠转义，CR/LF 用 13U/10U 配 %c%c
 *    c) 谁的外设谁碰：I2C1/ADC1→Sensor，FSMC→Display，UART4→Wifi，CAN1→Can，KEY→Key
 *============================================================================*/

/* ---- 角色 ---- */
#define IS_MASTER          (BOARD_SEL == 1)
#define IS_SLAVE           (BOARD_SEL != 1)
/* 本板板号字符（'1' / '2'）—— H/D/S 帧都带它，PC 据此区分来源 */
#define APP_BOARD_CH       ((u8)('0' + BOARD_SEL))

/* ---- WiFi 链路 ---- */
#define WIFI_LINK_PC       0U       /* 两块板都只有这一条：到 PC */

/* ---- 时序 ---- */
#define APP_SAMPLE_MS      1000U    /* 采样周期 1Hz（ESP-01 实测吞吐天花板 ≈1.5 次 CIPSEND/s）*/
#define APP_KEY_SCAN_MS     10U
#define APP_DISPLAY_MS     200U
#define APP_TX_GAP_MS      120U
#define APP_CAN_TICK_MS     10U     /* CanTask 周期（收发+状态机）*/
#define APP_CAN_HB_MS     1000U     /* 从机心跳周期（让板1 知道自己在）*/
#define APP_PEER_TIMEOUT_MS 3000U   /* 3s 没消息判离线 */
#define APP_SENSOR_FAIL_MAX  3U     /* 传感器连续失败几次 → 自动恢复 I2C 总线 */
#define APP_AI_MS         1000U     /* ★ 题2：AI 推理周期（与采样同频，1Hz）*/

/* ---- 温度报警（★ 注意：只有板1 判，见采样任务里的 #if IS_MASTER）----
 *
 *  默认 35℃（**原来是 28℃**）：
 *    室温常年在 27~28℃，阈值设 28 等于"手一碰就 ALM"，而
 *    tools/export_dataset.py 默认剔除 ALM 行 ⇒ 数据白采、板1 可能一条都不剩。
 *    改成 35℃ 后**上电即用，不用再手敲 `a`**（原来每次上电两块板各敲一次，
 *    而且板2 敲了根本没用 —— 它不参与 ALM 判定，纯属多余动作）。
 *
 *  演示「超温报警」时：
 *    手捂传感器到 36℃ 即可（阈值 35 + 滞回 1）；或者串口敲 `a 28` 立刻触发。
 *    演示完敲 `a 35`（或直接复位）恢复。*/
#define APP_ALARM_TEMP_C    35U
#define APP_ALARM_HYST_C     1U
#define APP_ALARM_TEMP_X10  (APP_ALARM_TEMP_C * 10U)
#define APP_ALARM_HYST_X10  (APP_ALARM_HYST_C * 10U)
#define APP_ALARM_BLINK_MS  300U

/*==============================================================================
 *  ★ 界面布局（240x320，全屏缩小字号 + 表格式排版）
 *
 *  字号（实验六缩小口径）：
 *      中文 = **16x16**（chinese16.c，占 16x16）；
 *      ASCII = **12 号**（cfont.h 的 char_1206，每字 6 宽 x 12 高）。
 *  行距 **24**（16 高 + 8 间隙）→ 一屏排 **12 行**（y = 6,30,54,...,270）。
 *
 *  真个为什么：实验五用 24 号中文（占 26x26）+ 行距 28，只排得下 10 行；
 *  实验六要显示三块板 · 5 项数据，必须缩到 16 号中文 + 12 号 ASCII。
 *----------------------------------------------------------------------------*/
#define ASC_SIZE   12U      /* ★ 全屏 ASCII 字号（只有这一处定义）*/
#define ASC_W       6U      /* 12 号 ASCII 的步进宽（= ASC_SIZE/2）*/
#define CN_W       16U      /* 16x16 中文的步进宽 */
#define ROW_H      24U      /* 行距 */

#define X_LABEL     6U      /* 中文标签起点（温度/湿度/…，2 字到 38）*/
#define X_COLON    42U      /* ':' */
#define X_VALUE    52U      /* 数值起点 */
#define X_UNIT     92U      /* 单位起点（C / %RH / % / ppm / ppb）*/
#define X_NUM      32U      /* 纯数字（序号）起点 */
#define X_TAG      40U      /* 「板N 正常/错误」里状态字起点 */

/* 补充颜色（lcd.h 没有橙色）*/
#define ORANGE     0xFD20U

/* 12 行的行首 y */
static const u16 Y_ROW[12] =
{
     6U,  30U,  54U,  78U, 102U, 126U,
   150U, 174U, 198U, 222U, 246U, 270U
};

/* ★ 「本板 5 项数据」（温度/湿度/光敏/CO2/甲醛）的首行：
     板1 数据页顶格就是温度 → 行 0..4；
     板2 数据页行 0 是「本板」小标题 → 数据在行 1..5。
   （原先写死行 0..4，从机页整列数值上移一行：本板标题行冒出温度值、
     甲醛行只剩单位没有数值。统一用本宏寻址。）*/
#if IS_MASTER
#define OWN_ROW0   0U
#else
#define OWN_ROW0   1U
#endif

/*==============================================================================
 *  状态中心（所有跨任务的数据都在这，读写都要拿锁）
 *============================================================================*/
typedef struct
{
    /* ---- 本板传感器（5 项）---- */
    u8  running;        /* 采集开关 */
    u32 count;          /* 采样序号 */
    s32 temp10;         /* 温度 x10 */
    s32 humi10;         /* 湿度 x10 */
    u32 light;          /* 光敏原始值 0..4095 */
    u32 eco2;           /* SGP30 eCO2 ppm */
    u32 tvoc;           /* SGP30 TVOC ppb */
    u8  sensor_ok;      /* SHT30 本次是否读成功 */
    u8  sgp_ready;      /* SGP30 是否走完预热 */
    u8  sgp_ok;         /* SGP30 本次是否读成功 */
    u8  alarm;          /* 超温报警（仅板1）*/
    u8  wifi_ok;        /* 是否连上上位机 */
    u8  msg;            /* 1=开始采集 2=停止采集 3=按键错误 */
    u8  ctrl_pending;   /* 板1：待发 CAN 控制帧 1=START 2=STOP */
    u8  greeted;        /* 本次连接是否已发过握手帧 */
    u8  page;           /* ★ 显示页：0=数据页 1=状态页（按键2 切）*/
    u8  sht_fail;       /* SHT30 连续失败次数 */
    u8  sgp_fail;       /* SGP30 连续失败次数 */
    u8  i2c_recover;    /* ★ 请求恢复 I2C 总线（按键3 手动 / 自动触发）*/
    u8  resend;         /* ★ 板1：请求重发一次 CAN 数据（按键3）*/

    /* ---- 板1（主控）记录从机在线 ---- */
    u8  b2_alive;
    /* ★ 实验七：只有两块板，"板3 在线"字段已删除（实验六是三板）。
       注意 CanTask 里 `m.data[0] == 3U` 的分支也要一并删掉 —— 否则收到
       编号 3 的心跳会写进一个不存在的节点，看起来"板3 在线"永远为假。 */

    /* ---- 板2/3（从机）收到的板1 数据 ---- */
    u8  b1_on;          /* 收到 CAN CTRL START（打开「板1区」+ 呼吸灯）*/
    u8  b1_alive;       /* 板1 数据/心跳未超时 */
    u8  b1_have;        /* 收到过板1 数据帧 */
    u32 b1_seq;
    s32 b1_temp10;
    s32 b1_humi10;
    u32 b1_light;
    u32 b1_eco2;
    u32 b1_tvoc;
    u8  b1_ok;

    /* ---- ★ 题2：对板原始值（统一入口，两板都用这几个字段喂 AI）----
       板1：来自 CAN 0x110/0x111（板2 回传）；板2：来自 CAN 0x100/0x101（板1 广播）。
       做成一套字段，AiTask 就不用关心自己是板1 还是板2。 */
    u8  peer_have;      /* 收到过对板数据 */
    u32 peer_seq;       /* 对板采样序号（板1 侧来自 0x110；板2 侧就是 b1_seq）*/
    s32 peer_temp10;
    s32 peer_humi10;
    u32 peer_light;
    u32 peer_eco2;
    u32 peer_tvoc;
    u8  peer_ok;

    /* ---- ★ 题2：神经网络判定结果 ---- */
    u8  ai_ready;       /* 模型是否已集成（Ai_Ready()）*/
    u8  ai_own_cls;     /* 本板数据 → 判为哪块板（1/2；0=未判）*/
    u8  ai_own_conf;    /* 置信度 0..100 */
    u8  ai_peer_cls;    /* ★ 对板数据（经 CAN 来的）→ 判定（交叉验证用）*/
    u8  ai_peer_conf;
} app_state_t;

static app_state_t       g;
static SemaphoreHandle_t g_mtx;          /* g 的互斥锁 */
static QueueHandle_t     q_sample;       /* SensorTask → WifiTask 的样本队列 */
static u32  g_can_tx_cnt = 0U;           /* 板1：CAN 数据帧广播次数（调试）*/
static u32  g_can_rx_cnt = 0U;           /* 板2：收到 CAN 帧数（调试）*/
static s32  g_alarm_thr_x10 = (s32)APP_ALARM_TEMP_X10;

#define LOCK()    (void)xSemaphoreTake(g_mtx, portMAX_DELAY)
#define UNLOCK()  (void)xSemaphoreGive(g_mtx)

/*------------------------------------------------------------------------------
 *  周期性延时（不漂移），替代 vTaskDelayUntil（见实验五注释：不动生成代码）。
 *----------------------------------------------------------------------------*/
static void TaskWaitPeriod(TickType_t *last, TickType_t period)
{
    TickType_t now  = xTaskGetTickCount();
    TickType_t next = *last + period;

    if ((s32)(next - now) > 0)
    {
        vTaskDelay(next - now);
    }
    *last = next;
}

/* 队列元素：一帧样本（含采样时刻 up_ms，PC 靠它算时间戳）*/
typedef struct
{
    u32 up_ms;
    u32 count;
    s32 temp10;
    s32 humi10;
    u32 light;
    u32 eco2;
    u32 tvoc;
    u8  alarm;
} sample_t;

static const u8  stu_name_gb[] = STU_NAME_GB;    /* GB18030，给串口/上位机 */
static const u8  stu_name_hz[] = STU_NAME_HZ;    /* ★ 题3：24x24 字库下标，给 LCD 署名 */

/*==============================================================================
 *  LCD 小工具（只有 DisplayTask 会调）
 *  字号：中文 16x16（步进 16）、ASCII 12（步进 6）
 *============================================================================*/
static void App_CN1(u16 x, u16 y, u8 idx)
{
    LCD_ShowChinese16(x, y, idx);
}

static void App_CN2(u16 x, u16 y, u8 a, u8 b)
{
    LCD_ShowChinese16(x, y, a);
    LCD_ShowChinese16((u16)(x + CN_W), y, b);
}

static void App_CN4(u16 x, u16 y, u8 a, u8 b, u8 c, u8 d)
{
    LCD_ShowChinese16(x, y, a);
    LCD_ShowChinese16((u16)(x + CN_W),     y, b);
    LCD_ShowChinese16((u16)(x + CN_W * 2U), y, c);
    LCD_ShowChinese16((u16)(x + CN_W * 3U), y, d);
}

/* 用背景色把 [x0, x1) 这段 ASCII 区域擦净（12 号字，每字 6 宽）*/
static void App_BlankAscii(u16 x0, u16 y, u16 x1)
{
    while (x0 < x1)
    {
        LCD_DisplayChar(x0, y, ' ', ASC_SIZE);
        x0 = (u16)(x0 + ASC_W);
    }
}

/* 把 x10 定点数按 [-]d.d 显示（12 号 ASCII）。先擦 [x, x+maxc*6) 再画。*/
static void App_ShowFix1(u16 x, u16 y, s32 v10, u8 maxc)
{
    char   b[10];
    u8     n = 0U;
    u32    a = (u32)((v10 < 0) ? -v10 : v10);
    u32    ip = a / 10U;
    u32    fp = a % 10U;
    char   t[6];
    u8     k = 0U;

    App_BlankAscii(x, y, (u16)(x + (u16)maxc * ASC_W));

    if (ip == 0U) { t[k] = '0'; k++; }
    while ((ip != 0U) && (k < 6U)) { t[k] = (char)('0' + (ip % 10U)); ip /= 10U; k++; }

    if (v10 < 0) { b[n] = '-'; n++; }
    while (k != 0U) { k--; b[n] = t[k]; n++; }
    b[n] = '.'; n++;
    b[n] = (char)('0' + fp); n++;
    b[n] = '\0';

    /* ★ 在 maxc 个字符的字段里**右对齐**：这样小数点和单位列始终对齐，
       不会出现 "25.3 C" 和 " 5.2 C" 参差不一的情况。 */
    if (n < maxc)
    {
        LCD_DisplayString((u16)(x + (u16)(maxc - n) * ASC_W), y, ASC_SIZE, (u8 *)b);
    }
    else
    {
        LCD_DisplayString(x, y, ASC_SIZE, (u8 *)b);
    }
}

/* 无符号数：先擦 len 个字符宽，再显示。
   pad0=1 → 前导补 0（序号/计数器，位对齐好看）
   pad0=0 → 前导留空（★ 测量值用这个：光敏 45 显示 " 45"，不要 "045"）*/
static void App_ShowNum(u16 x, u16 y, u32 v, u8 len, u8 pad0)
{
    App_BlankAscii(x, y, (u16)(x + (u16)len * ASC_W));
    LCD_DisplayNum(x, y, v, len, ASC_SIZE, pad0);
}

/* 状态词（正常 / 错误）—— ★ 上色：正常=绿、错误=红，一眼看得出好坏。
   画完恢复 BRUSH_COLOR，不影响后续绘制（报警闪烁时也会正确叠加）。 */
static void App_ShowOK(u16 x, u16 y, u8 ok)
{
    u16 save = BRUSH_COLOR;
    BRUSH_COLOR = (ok != 0U) ? GREEN : RED;
    if (ok != 0U) { App_CN2(x, y, H16_ZHENG, H16_CHANG); }
    else          { App_CN2(x, y, H16_CUO,   H16_WU); }
    BRUSH_COLOR = save;
}

/* 消息（开始采集 / 停止采集 / 按键错误）*/
static void App_ShowMsg(u16 x, u16 y, u8 which)
{
    if (which == 1U)      { App_CN4(x, y, H16_KAI,  H16_SHI2, H16_CAI, H16_JI); }
    else if (which == 2U) { App_CN4(x, y, H16_TING, H16_ZHI,  H16_CAI, H16_JI); }
    else if (which == 3U) { App_CN4(x, y, H16_AN,   H16_JIAN, H16_CUO, H16_WU); }
    else                  { App_BlankAscii(x, y, (u16)(x + 4U * CN_W)); }  /* 0=擦 */
}

/* 一条水平分隔线 */
static void App_HLine(u16 y, u16 color)
{
    LCD_Fill_onecolor(6U, y, (u16)(240U - 7U), y, color);
}

/* 「本板」小标题（2 中文，可上色）*/
static void App_Title2(u16 y, u8 a, u8 b, u16 color)
{
    BRUSH_COLOR = color;
    App_CN2(6U, y, a, b);
    BRUSH_COLOR = BLUE;
}

/* 「板1」小标题（中文"板" + ASCII '1'，可上色）*/
static void App_TitleB1(u16 y, u16 color)
{
    BRUSH_COLOR = color;
    App_CN1(6U, y, H16_BAN);
    LCD_DisplayChar((u16)(6U + CN_W), y, '1', ASC_SIZE);
    BRUSH_COLOR = BLUE;
}

/*==============================================================================
 *  ★ 题3：屏底部常驻署名（姓名 + 学号）
 *
 *  放在 y=290（屏 240x320）。数据页/状态页的动态刷新只覆盖 Y_ROW[0..11]，
 *  最下一行是 Y_ROW[11]=270 的 12 号字（占 270..282），**不会碰 290**，
 *  所以这个署名每个页面只需在「清屏之后」画一次，不必每周期重画：
 *     · 数据页：App_DrawStatic() 末尾（它自己先 LCD_Clear）
 *     · 状态页：DisplayTask 里换页清屏后补画一次
 *  换人只改 app_id.h 里的 STU_WHO，姓名点阵与学号一起跟着变。
 *============================================================================*/
#define Y_SIGN      290U        /* 署名行行首 y */
#define SIGN_CNW    26U         /* 24x24 中文字模的步进宽 */
#define SIGN_ID_FS  16U         /* 学号用 16 号 ASCII（步进 = 字号 / 2 = 8）*/

static void App_DrawId(void)
{
    u8  i;
    u16 n = (u16)(sizeof(STU_ID) - 1U);                 /* 学号字符数（去掉 '\0'）*/
    u16 w = (u16)(STU_NAME_HZ_N * SIGN_CNW + 8U + n * (SIGN_ID_FS / 2U));
    u16 x = (u16)((240U - w) / 2U);                     /* 整行水平居中 */

    BRUSH_COLOR = BLUE;

    for (i = 0U; i < STU_NAME_HZ_N; i++)
    {
        LCD_DisplayChinese((u16)(x + SIGN_CNW * i), Y_SIGN, stu_name_hz[i]);
    }

    x = (u16)(x + STU_NAME_HZ_N * SIGN_CNW + 8U);
    /* 16 号字高 16、中文高 24 ⇒ 下移 4 使两者视觉居中 */
    LCD_DisplayString(x, (u16)(Y_SIGN + 4U), SIGN_ID_FS, (u8 *)STU_ID);
}

/*==============================================================================
 *  静态骨架（开机画一次）
 *============================================================================*/
/* ★ 题2 的 AI 判定显示函数在下面才定义，这里先声明（它在 App_RefreshDisplay
   里被调用，而 App_RefreshDisplay 在这个位置之前）—— 否则 ARMCC 报
   "declared implicitly" 并把它当成返回 int 的隐式函数。*/
static void App_ShowAIPair(u16 x, u16 y);

static void App_DrawStatic(void)
{
    LCD_Clear(WHITE);
    BRUSH_COLOR = BLUE;
    BACK_COLOR  = WHITE;

#if IS_MASTER
    /* ---- 板1：本板 5 项 + 状态 + CAN 从机 + 底栏 ---- */
    App_CN2(X_LABEL, Y_ROW[0], H16_WEN,  H16_DU);      /* 温度 */
    App_CN2(X_LABEL, Y_ROW[1], H16_SHI,  H16_DU);      /* 湿度 */
    App_CN2(X_LABEL, Y_ROW[2], H16_GUANG, H16_MIN);    /* 光敏 */
    LCD_DisplayString(X_LABEL, Y_ROW[3], ASC_SIZE, (u8 *)"CO2");   /* CO2 */
    App_CN2(X_LABEL, Y_ROW[4], H16_JIA,  H16_QUAN);    /* 甲醛 */

    App_HLine((u16)(Y_ROW[4] + 20U), GRAY);

    LCD_DisplayString(X_LABEL, Y_ROW[6], ASC_SIZE, (u8 *)"NO");    /* 序号 */
    App_CN2(X_LABEL, Y_ROW[7], H16_ZHUANG, H16_TAI);   /* 状态 */
    /* ★ 只画一个「板」，后面补 ASCII '2'。
       曾误用 App_CN2(..., H16_BAN, 0U)：第二个参数 0U 被当成字模下标，
       0 = H16_WEN「温」，于是在 '2'/'3' 右边多出一个「温」。用 App_CN1 画单字。 */
    App_CN1(X_LABEL, Y_ROW[8], H16_BAN);               /* 板2 */
    LCD_DisplayChar((u16)(X_LABEL + CN_W), Y_ROW[8], '2', ASC_SIZE);
    /* ★ 实验七：行 9 原为「板3 在线」（实验六三板），已删。
       行 9 留给阶段5 的 AI 判定显示（"板? / 主特征"），暂不画。 */

    App_HLine((u16)(Y_ROW[9] + 20U), GRAY);

    /* 单位（静态）*/
    LCD_DisplayChar(X_UNIT, Y_ROW[0], 'C', ASC_SIZE);
    LCD_DisplayString(X_UNIT, Y_ROW[1], ASC_SIZE, (u8 *)"%RH");
    LCD_DisplayChar(X_UNIT, Y_ROW[2], '%', ASC_SIZE);
    LCD_DisplayString(X_UNIT, Y_ROW[3], ASC_SIZE, (u8 *)"ppm");
    LCD_DisplayString(X_UNIT, Y_ROW[4], ASC_SIZE, (u8 *)"ppb");

    /* 底栏：WiFi + 消息 */
    LCD_DisplayString(X_LABEL, Y_ROW[11], ASC_SIZE, (u8 *)"WiFi");
    App_CN2(96U, Y_ROW[11], H16_XIAO, H16_XI);          /* 消息 */
#else
    /* ---- 板2：本板区（5 行）+ 板1 区（5 行）---- */
    App_Title2(Y_ROW[0], H16_BEN, H16_BAN, BLUE);       /* 本板 */
    App_CN2(X_LABEL, Y_ROW[1], H16_WEN,   H16_DU);
    App_CN2(X_LABEL, Y_ROW[2], H16_SHI,   H16_DU);
    App_CN2(X_LABEL, Y_ROW[3], H16_GUANG, H16_MIN);
    LCD_DisplayString(X_LABEL, Y_ROW[4], ASC_SIZE, (u8 *)"CO2");
    App_CN2(X_LABEL, Y_ROW[5], H16_JIA,   H16_QUAN);

    App_HLine((u16)(Y_ROW[5] + 20U), GRAY);

    /* 板1 区标题（初值灰；有数据后由 App_RefreshDisplay 上色）*/
    App_TitleB1(Y_ROW[6], GRAY);

    App_CN2(X_LABEL, Y_ROW[7],  H16_WEN,   H16_DU);
    App_CN2(X_LABEL, Y_ROW[8],  H16_SHI,   H16_DU);
    App_CN2(X_LABEL, Y_ROW[9],  H16_GUANG, H16_MIN);
    LCD_DisplayString(X_LABEL, Y_ROW[10], ASC_SIZE, (u8 *)"CO2");
    App_CN2(X_LABEL, Y_ROW[11], H16_JIA,   H16_QUAN);

    /* 单位（本板区 + 板1 区）*/
    LCD_DisplayChar(X_UNIT, Y_ROW[1], 'C', ASC_SIZE);
    LCD_DisplayString(X_UNIT, Y_ROW[2], ASC_SIZE, (u8 *)"%RH");
    LCD_DisplayChar(X_UNIT, Y_ROW[3], '%', ASC_SIZE);
    LCD_DisplayString(X_UNIT, Y_ROW[4], ASC_SIZE, (u8 *)"ppm");
    LCD_DisplayString(X_UNIT, Y_ROW[5], ASC_SIZE, (u8 *)"ppb");
    LCD_DisplayChar(X_UNIT, Y_ROW[7], 'C', ASC_SIZE);
    LCD_DisplayString(X_UNIT, Y_ROW[8], ASC_SIZE, (u8 *)"%RH");
    LCD_DisplayChar(X_UNIT, Y_ROW[9], '%', ASC_SIZE);
    LCD_DisplayString(X_UNIT, Y_ROW[10], ASC_SIZE, (u8 *)"ppm");
    LCD_DisplayString(X_UNIT, Y_ROW[11], ASC_SIZE, (u8 *)"ppb");
#endif

    /* ★ 这里原有一段「数值区初值」循环：
     *     for (i = 0; i < 12; i++) App_BlankAscii(X_VALUE, Y_ROW[i], X_UNIT + 18);
     *   它是多余且有害的，已删除：
     *   1) 本函数开头已 LCD_Clear(WHITE) 整屏清过，不存在上一轮残影；
     *   2) 它按 [52, 110) 擦 12 行，会吃掉刚画好的静态字 ——
     *      底栏「消息」的「消」正好画在 x=96..112，被整字擦掉，
     *      只留下 x=112..128 的「息」（= 用户看到的「消这个字没了」）。
     *   3) 所有动态字段（App_ShowFix1 / App_ShowNum / App_ShowOK / App_ShowMsg）
     *      在画之前都会按各自固定宽度先擦，不需要这里兜底。
     */
    App_DrawId();       /* ★ 题3：屏底部常驻署名（本函数已先清屏，这里画一次）*/
}

/*==============================================================================
 *  刷新动态数值
 *============================================================================*/
static void App_RefreshDisplay(void)
{
    app_state_t s;
    u8 pct;
#if IS_MASTER
    static u8  blink_on = 0U;
    static u32 blink_t0 = 0U;
    u32 now = HAL_GetTick();
#endif

    LOCK(); s = g; UNLOCK();            /* 快照，锁外再画（画屏慢，别占着锁）*/

    if (s.sensor_ok == 0U)
    {
        /* 传感器没读上：数值画 "--" */
        App_BlankAscii(X_VALUE, Y_ROW[OWN_ROW0 + 0U], X_UNIT);
        App_BlankAscii(X_VALUE, Y_ROW[OWN_ROW0 + 1U], X_UNIT);
        App_BlankAscii(X_VALUE, Y_ROW[OWN_ROW0 + 2U], X_UNIT);
        LCD_DisplayString(X_VALUE, Y_ROW[OWN_ROW0 + 0U], ASC_SIZE, (u8 *)"--");
        LCD_DisplayString(X_VALUE, Y_ROW[OWN_ROW0 + 1U], ASC_SIZE, (u8 *)"--");
        LCD_DisplayString(X_VALUE, Y_ROW[OWN_ROW0 + 2U], ASC_SIZE, (u8 *)"--");
    }
    else
    {
        App_ShowFix1(X_VALUE, Y_ROW[OWN_ROW0 + 0U], s.temp10, 5U);
        App_ShowFix1(X_VALUE, Y_ROW[OWN_ROW0 + 1U], s.humi10, 5U);
        pct = (u8)(((s.light * 100U) + 2047U) / 4095U);
        App_ShowNum(X_VALUE, Y_ROW[OWN_ROW0 + 2U], (u32)pct, 3U, 0U);
    }

    /* CO2 / 甲醛：★ 两个条件都要满足才显示数值，否则 "---"
          sgp_ready = SGP30 已走完 ~15s 预热；
          running    = 本板确实在采集（SGP30 现在是"与采集开关无关、一直在维温"，
                       所以光看 sgp_ready 会在"还没按开始采集"时就跳数，
                       与上面温度/湿度显示"--"的观感不一致）。*/
    if ((s.running != 0U) && (s.sgp_ready != 0U))
    {
        App_ShowNum(X_VALUE, Y_ROW[OWN_ROW0 + 3U], s.eco2, 5U, 0U);
        App_ShowNum(X_VALUE, Y_ROW[OWN_ROW0 + 4U], s.tvoc, 5U, 0U);
    }
    else
    {
        App_BlankAscii(X_VALUE, Y_ROW[OWN_ROW0 + 3U], (u16)(X_UNIT));
        App_BlankAscii(X_VALUE, Y_ROW[OWN_ROW0 + 4U], (u16)(X_UNIT));
        LCD_DisplayString(X_VALUE, Y_ROW[OWN_ROW0 + 3U], ASC_SIZE, (u8 *)"---");
        LCD_DisplayString(X_VALUE, Y_ROW[OWN_ROW0 + 4U], ASC_SIZE, (u8 *)"---");
    }

#if IS_MASTER
    App_ShowNum(X_NUM, Y_ROW[6], s.count, 4U, 1U);          /* 序号 */
    App_ShowOK(X_VALUE, Y_ROW[7], s.sensor_ok);         /* 状态 */
    App_ShowOK(X_TAG,  Y_ROW[8], s.b2_alive);           /* 板2 在线（实验七只有两块板）*/
    App_ShowAIPair(6U, Y_ROW[9]);                       /* ★ 题2：AI 判定（行 9 原为「板3 在线」）*/

    App_ShowOK(X_TAG, Y_ROW[11], s.wifi_ok);            /* WiFi */
    App_ShowMsg(128U, Y_ROW[11], s.msg);                /* 消息 */

    /* 报警：蜂鸣器 + 序号行颜色一起闪（仅板1）*/
    if (s.alarm == 0U)
    {
        if (blink_on != 0U)
        {
            blink_on = 0U;
            BEEP = 0;
            BRUSH_COLOR = BLUE;
        }
    }
    else if ((now - blink_t0) >= APP_ALARM_BLINK_MS)
    {
        blink_t0 = now;
        blink_on  = (u8)((blink_on == 0U) ? 1U : 0U);
        BEEP      = blink_on;
        BRUSH_COLOR = (blink_on != 0U) ? RED : BLUE;
    }
#else
    /* 板1 区标题上色 + 数值 */
    /* ★ 题2：AI 判定 —— 板2 的数据页没有空行，放在「本板」标题（行 0）右侧 */
    App_ShowAIPair(56U, Y_ROW[0]);

    if (s.b1_on == 0U)
    {
        App_TitleB1(Y_ROW[6], GRAY);                    /* 关闭：灰 */
    }
    else if (s.b1_alive == 0U)
    {
        App_TitleB1(Y_ROW[6], ORANGE);                  /* 未连接：橙 */
    }
    else
    {
        App_TitleB1(Y_ROW[6], GREEN);                   /* 数据刷新中：绿 */
    }

    if ((s.b1_on == 0U) || (s.b1_have == 0U))
    {
        u8 r;
        for (r = 7U; r < 12U; r++)
        {
            App_BlankAscii(X_VALUE, Y_ROW[r], X_UNIT);
            LCD_DisplayString(X_VALUE, Y_ROW[r], ASC_SIZE, (u8 *)"--");
        }
    }
    else
    {
        App_ShowFix1(X_VALUE, Y_ROW[7],  s.b1_temp10, 5U);
        App_ShowFix1(X_VALUE, Y_ROW[8],  s.b1_humi10, 5U);
        pct = (u8)(((s.b1_light * 100U) + 2047U) / 4095U);
        App_ShowNum (X_VALUE, Y_ROW[9],  (u32)pct, 3U, 0U);
        App_ShowNum (X_VALUE, Y_ROW[10], s.b1_eco2, 5U, 0U);
        App_ShowNum (X_VALUE, Y_ROW[11], s.b1_tvoc, 5U, 0U);
    }
#endif
}

/*==============================================================================
 *  建帧（只有 WifiTask 会调）
 *============================================================================*/
static u16 App_AppendU32(u8 *dst, u32 value)
{
    u8  tmp[10];
    u16 k = 0U;
    u16 i;

    if (value == 0U)
    {
        dst[0] = (u8)48;
        return 1U;
    }
    while ((value != 0U) && (k < 10U))
    {
        tmp[k] = (u8)(48U + (value % 10U));
        value /= 10U;
        k++;
    }
    for (i = 0U; i < k; i++)
    {
        dst[i] = tmp[k - 1U - i];
    }
    return k;
}

/* H,<姓名>,<学号>,<board> */
static u16 App_BuildHandshake(u8 *out)
{
    u16 n = 0U;
    u8  i;

    out[n] = (u8)'H'; n++;
    out[n] = (u8)','; n++;
    for (i = 0U; i < STU_NAME_GB_N; i++) { out[n] = stu_name_gb[i]; n++; }
    out[n] = (u8)','; n++;
    {
        const char *sid = STU_ID;
        while (*sid != '\0') { out[n] = (u8)(*sid); n++; sid++; }
    }
    out[n] = (u8)','; n++;
    out[n] = APP_BOARD_CH; n++;
    out[n] = 13U; n++;
    out[n] = 10U; n++;
    return n;
}

/* D,<board>,<up_ms>,<seq>,<t>.<1>,<h>.<1>,<light>,<eco2>,<tvoc>,<OK/ALM>[,<ai_cls>,<ai_conf>]
   —— 协议 v6 打底，v7 在尾部追加 AI 判定（题2）。
   ★ ai_cls / ai_conf **总是**追加：没集成模型时 ai_cls=0，上位机按"未推理"显示 "--"。
     这样上位机不用判断字段数，协议也不会出现"有时 10 个字段有时 12 个"的歧义。 */
static u16 App_BuildDataFrame(u8 *out, u32 up_ms, u32 index,
                              s32 temp10, s32 humi10, u32 light,
                              u32 eco2, u32 tvoc, u8 alarm,
                              u8 ai_cls, u8 ai_conf)
{
    u16 n = 0U;
    u32 t_abs;

    out[n] = (u8)'D'; n++;
    out[n] = (u8)','; n++;
    out[n] = APP_BOARD_CH; n++;
    out[n] = (u8)','; n++;
    n = (u16)(n + App_AppendU32(&out[n], up_ms));
    out[n] = (u8)','; n++;
    n = (u16)(n + App_AppendU32(&out[n], index));
    out[n] = (u8)','; n++;

    t_abs = (u32)((temp10 < 0) ? -temp10 : temp10);
    if (temp10 < 0) { out[n] = (u8)'-'; n++; }
    n = (u16)(n + App_AppendU32(&out[n], t_abs / 10U));
    out[n] = (u8)'.'; n++;
    n = (u16)(n + App_AppendU32(&out[n], t_abs % 10U));
    out[n] = (u8)','; n++;

    n = (u16)(n + App_AppendU32(&out[n], (u32)(humi10 / 10)));
    out[n] = (u8)'.'; n++;
    n = (u16)(n + App_AppendU32(&out[n], (u32)(humi10 % 10)));
    out[n] = (u8)','; n++;

    n = (u16)(n + App_AppendU32(&out[n], light));
    out[n] = (u8)','; n++;
    n = (u16)(n + App_AppendU32(&out[n], eco2));
    out[n] = (u8)','; n++;
    n = (u16)(n + App_AppendU32(&out[n], tvoc));
    out[n] = (u8)','; n++;

    if (alarm != 0U) { out[n]=(u8)'A'; n++; out[n]=(u8)'L'; n++; out[n]=(u8)'M'; n++; }
    else             { out[n]=(u8)'O'; n++; out[n]=(u8)'K'; n++; }

    /* ★ v7：AI 判定（题2）—— 0 = 未推理；1 = 板1；2 = 板2 */
    out[n] = (u8)','; n++;
    n = (u16)(n + App_AppendU32(&out[n], (u32)ai_cls));
    out[n] = (u8)','; n++;
    n = (u16)(n + App_AppendU32(&out[n], (u32)ai_conf));

    out[n] = 13U; n++;
    out[n] = 10U; n++;
    return n;
}

/* S,<board>,START|STOP  —— 协议 v7 新增：**采集启停状态上报**（题1）
   题1 要求"web 页面显示采集开始或停止"。约定：
     - 状态**发生变化**（板1 按 KEY0/KEY1）时立刻发一条；
     - 刚连上 PC 时**补发一次当前状态** —— 否则网页要等用户按键才知道板子
       到底有没有在采集（板2 上电即采，开机就应显示"采集开始"）。
   与 D 帧同走 link0，队列顺序天然保证"先 H 后 S"。 */
static u16 App_BuildStatusFrame(u8 *out, u8 running)
{
    u16 n = 0U;

    out[n] = (u8)'S'; n++;
    out[n] = (u8)','; n++;
    out[n] = APP_BOARD_CH; n++;
    out[n] = (u8)','; n++;
    if (running != 0U)
    {
        out[n]=(u8)'S'; n++; out[n]=(u8)'T'; n++; out[n]=(u8)'A'; n++;
        out[n]=(u8)'R'; n++; out[n]=(u8)'T'; n++;
    }
    else
    {
        out[n]=(u8)'S'; n++; out[n]=(u8)'T'; n++; out[n]=(u8)'O'; n++;
        out[n]=(u8)'P'; n++;
    }
    out[n] = 13U; n++;
    out[n] = 10U; n++;
    return n;
}

/*==============================================================================
 *  I2C 总线恢复
 *  ★ 传感器没接 / 线松 / 总线被拉死时，I2C 外设可能卡在 BUSY 出不来 ——
 *    之后每次读都秒失败，看着就像"整个模块不干活了"。
 *    这里 DeInit + Init 一次，把外设状态机清干净（MspInit 会重新配引脚+上拉）。
 *  触发：① SensorTask 连续失败达阈值  ② 按键3 手动
 *============================================================================*/
/* 重置一条 I2C 总线：DeInit + Init（MspInit 会重配引脚 + 上拉）。
   返回 0 = OK，1 = 失败。用 HAL_I2C_* 而非 MX_I2Cx_Init（后者失败会 Error_Handler）。*/
static u8 App_I2CBusReset(I2C_HandleTypeDef *h)
{
    (void)HAL_I2C_DeInit(h);
    return (u8)((HAL_I2C_Init(h) == HAL_OK) ? 0U : 1U);
}

static void App_I2CRecover(void)
{
    /* ★ 用 HAL_I2C_Init 而**不是** MX_I2Cx_Init：
       MX_I2Cx_Init 失败时会调 Error_Handler()（关中断 + 死循环），
       而"总线卡住"恰恰就是我们要恢复的场景 —— 那会把可恢复的传感器故障
       变成整机死机。HAL_I2C_Init 只返回状态码：恢复不成功也只是这轮没成，
       下一轮再试，系统照常跑。
       （h 的 Init 参数在 DeInit 后仍保留，故直接 Init 即可。）

       ★ SHT30 挂 I2C1、SGP30 挂 I2C2：单标志 i2c_recover 不区分是哪条坏的，
         故两条都重置一遍 —— 健康的那条 DeInit+Init 只是瞬间重同步、无副作用；
         本函数只在 SensorTask 内被调用，两条总线无并发用户，安全。*/
    u8 bad = App_I2CBusReset(&hi2c1);
    bad   |= App_I2CBusReset(&hi2c2);

    if (bad != 0U)
    {
        UART_Printf("[I2C] recover FAILED (bus still bad)%c%c", 13U, 10U);
        return;
    }
    UART_Printf("[I2C] bus recovered (I2C1 + I2C2 DeInit + Init)%c%c", 13U, 10U);
}

/*==============================================================================
 *  SensorTask：采集（SHT30 + 光敏 + SGP30）→ 写 g_state → 样本入队
 *  独占：I2C1（SHT30）+ I2C2（SGP30）、ADC1
 *  ★ 传感器初始化放在本任务里（不放 App_Init）：
 *      - 启动不被传感器拖住（LCD/按键/WiFi 立刻能用）
 *      - I2C 万一卡死，只卡这一个任务，其余照常跑，屏上能看出"传感器 错误"
 *============================================================================*/
static void SensorTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    u32 t_boot = HAL_GetTick();
    u8  inited = 0U;
    (void)arg;

    for (;;)
    {
        s32 t10 = 0;
        s32 h10 = 0;
        u32 light = 0U;
        u16 e16 = 0U;
        u16 v16 = 0U;
        u8  ok    = 0U;
        u8  sgp_ok = 0U;
        u8  run;
        sample_t s;

        TaskWaitPeriod(&last, pdMS_TO_TICKS(APP_SAMPLE_MS));

        /* ---- 传感器初始化：放任务里做（★ 不再阻塞启动）---- */
        if (inited == 0U)
        {
            UART_Printf("[SHT30] init = %u  (0=OK)%c%c", (u32)SHT30_Init(), 13U, 10U);
            UART_Printf("[SGP30] init = %u  (0=OK, warmup ~15s)%c%c", (u32)SGP30_Init(), 13U, 10U);
            Light_Init();
            UART_Printf("[LIGHT] AO init ok, raw = %u / 4095%c%c",
                        (u32)Light_ReadAvg(), 13U, 10U);
            t_boot = HAL_GetTick();             /* 预热从这一刻起算 */
            inited = 1U;
            continue;                           /* 这一拍先不采，下一拍开始 */
        }

        /* ---- I2C 总线恢复（按键3 手动请求 / 上面自动触发）---- */
        {
            u8 req;
            LOCK(); req = g.i2c_recover; g.i2c_recover = 0U; UNLOCK();
            if (req != 0U) { App_I2CRecover(); }
        }

        LOCK(); run = g.running; UNLOCK();

        /* ---- SGP30：★ 与「采集开关」**解耦**，上电后一直 1Hz 测 ----
               SGP30 的 IAQ 算法要求"初始化后持续每秒测一次"把基线收敛好；
               原来只在 run!=0 时才测 → 停机期间不测，一按开始采集要再等几秒
               才有可信读数（= 用户反馈的「启动采集后过几秒才开始采集」）。
               两段式：发命令 → vTaskDelay 睡着等 → 取结果（12ms 不占 CPU）。---- */
        if (SGP30_StartMeasure() == HAL_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(SGP30_MEASURE_WAIT_MS));
            sgp_ok = (u8)((SGP30_FetchIAQ(&e16, &v16) == HAL_OK) ? 1U : 0U);
        }

        /* SGP30 状态 / 失败计数 / 自动恢复 I2C —— 无论是否在采集都要维护 */
        {
            u8 ready = (u8)(((HAL_GetTick() - t_boot) >= SGP30_WARMUP_MS) ? 1U : 0U);
            u8 rec   = 0U;
            LOCK();
            if (sgp_ok != 0U) { g.sgp_fail = 0U; g.eco2 = e16; g.tvoc = v16; }
            else if (g.sgp_fail < 255U) { g.sgp_fail++; }
            if (g.sgp_fail >= APP_SENSOR_FAIL_MAX)
            {
                g.sgp_fail    = 0U;
                g.i2c_recover = 1U;         /* 下一拍恢复总线 */
                rec           = 1U;
            }
            g.sgp_ok    = sgp_ok;
            g.sgp_ready = (u8)((ready != 0U) && (sgp_ok != 0U) ? 1U : 0U);
            UNLOCK();
            if (rec != 0U)
            {
                UART_Printf("[SENSOR] SGP30 %u consecutive fails -> recover I2C%c%c",
                            (u32)APP_SENSOR_FAIL_MAX, 13U, 10U);
            }
        }

        if (run == 0U)
        {
            LOCK();
            g.alarm = 0U;
            UNLOCK();
            continue;                       /* 没在采集：不读 SHT30/光敏、不产样本 */
        }

        /* ---- SHT30 + 光敏 ---- */
        if (SHT30_ReadTempHumi10(&t10, &h10) == HAL_OK)
        {
            ok = 1U;
            light = Light_ReadAvg();
        }

        /* ---- SHT30 失败计数 → 连续失败就自动恢复 I2C1（防"卡死"）
               （SGP30 的失败计数已在上面维温块里处理）---- */
        {
            u8 auto_rec = 0U;
            LOCK();
            if (ok != 0U) { g.sht_fail = 0U; }
            else if (g.sht_fail < 255U) { g.sht_fail++; }
            if (g.sht_fail >= APP_SENSOR_FAIL_MAX)
            {
                g.sht_fail    = 0U;
                g.i2c_recover = 1U;         /* 下一拍恢复总线 */
                auto_rec      = 1U;
            }
            UNLOCK();
            if (auto_rec != 0U)
            {
                UART_Printf("[SENSOR] SHT30 %u consecutive fails -> recover I2C%c%c",
                            (u32)APP_SENSOR_FAIL_MAX, 13U, 10U);
            }
        }

        /* 本拍样本快照（SGP30 的 g.eco2/g.tvoc/sgp_* 已在上面的维温块里落地）*/
        {
            LOCK();
            g.count++;
            g.sensor_ok = ok;
            if (ok != 0U)
            {
                g.temp10 = t10;
                g.humi10 = h10;
                g.light  = light;
#if IS_MASTER
                if (t10 >= g_alarm_thr_x10) { g.alarm = 1U; }
                else if (t10 <= (g_alarm_thr_x10 - (s32)APP_ALARM_HYST_X10)) { g.alarm = 0U; }
#endif
            }
            s.up_ms  = HAL_GetTick();
            s.count  = g.count;
            s.temp10 = t10;
            s.humi10 = h10;
            s.light  = light;
            s.eco2   = e16;
            s.tvoc   = v16;
            s.alarm  = g.alarm;
            UNLOCK();
        }

        if (ok != 0U)
        {
            UART_Printf((const char *)TXT_SAMPLE6,
                        s.count,
                        (s32)(t10 / 10),
                        (s32)(((t10 < 0) ? -t10 : t10) % 10),
                        (s32)(h10 / 10),
                        (s32)(h10 % 10),
                        (u32)light, (u32)e16, (u32)v16);
            UART_Puts((const char *)TXT_OK);
            if (xQueueSend(q_sample, &s, 0) != pdTRUE) { /* 队列满：丢样本 */ }
        }
        else
        {
            UART_Printf((const char *)TXT_FAIL6, s.count);
        }
    }
}

/*==============================================================================
 *  App_SetRunning：★ 采集开关的**唯一入口**（实验七：两板等价）
 *
 *  为什么要有这么一个函数：实验七要求"按任意一板的 KEY0/KEY1 都能让**两板**
 *  同时开始/停止采集"。于是 running 有两个来源：
 *      ① 本板按键        → send_ctrl = 1（还要经 CAN 通知对板）
 *      ② 收到对板的 CTRL → send_ctrl = 0（**绝不能再发一次**，否则两板来回互相
 *                          转发，总线被自己的控制帧刷屏）
 *  两个入口都从这里走，才不会出现"某处漏改 running / 忘了同步 msg"的偏差。
 *
 *  顺带同步：
 *      g.msg   —— 屏上底栏的「开始采集/停止采集」提示
 *      g.b1_on —— 板2 用它开关「板1区」显示（板1 无影响）
 *      g.alarm —— 停止时解除报警
 *============================================================================*/
static void App_SetRunning(u8 run, u8 send_ctrl)
{
    LOCK();
    g.running = run;
    g.msg     = (u8)((run != 0U) ? 1U : 2U);
    g.b1_on   = run;                    /* 板2 用；板1 侧无意义，赋值无害 */
    if (run == 0U) { g.alarm = 0U; }
    UNLOCK();

    if (send_ctrl != 0U)
    {
        /* → CanTask 发 CTRL：板1 走 0x200、板2 走 0x201 */
        LOCK();
        g.ctrl_pending = (u8)((run != 0U) ? 1U : 2U);
        UNLOCK();
    }
}

/*==============================================================================
 *  CanTask：板间 CAN（两块板等价）
 *      两块板**都**：发心跳 + 发按键控制帧 + 收对板的控制帧
 *      板1 额外：广播本板 5 项数据（板2 的「板1区」显示）
 *      板2 额外：收数据帧 → 更新「板1区」
 *      ★ 实验七只有两块板（实验六三板）：原来"板3 心跳"的分支已删。
 *  独占：CAN1
 *============================================================================*/
static void CanTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    u32 hb_tx_t0  = HAL_GetTick();      /* 本板心跳发送计时 */
#if IS_MASTER
    u32 hb_b2_t   = 0U;                 /* 板1：上次收到板2 心跳的时刻 */
#else
    u32 b1_last_t = 0U;                 /* 板2：上次收到板1 数据帧的时刻 */
#endif
    (void)arg;

    for (;;)
    {
        can_msg_t m;
        u32 now;

        TaskWaitPeriod(&last, pdMS_TO_TICKS(APP_CAN_TICK_MS));
        now = HAL_GetTick();

        /* ---- 0) 按键触发的控制帧：★ 两块板**都**发（两板等价）----
               板1 发 0x200、板2 发 0x201；对板收到就跟着一起启停。
               ★ 发不出去就登记回去、下一拍再试 —— 别让按键白按。 */
        {
            u8 ctrl;
            LOCK(); ctrl = g.ctrl_pending; g.ctrl_pending = 0U; UNLOCK();
            if (ctrl != 0U)
            {
                if (Can_SendCtrl((u8)BOARD_SEL, (u8)((ctrl == 1U) ? 1U : 0U)) != 0U)
                {
                    UART_Printf("[CAN] -> %s (from b%u)%c%c",
                                (ctrl == 1U) ? "CTRL START" : "CTRL STOP",
                                (u32)BOARD_SEL, 13U, 10U);
                }
                else
                {
                    LOCK();
                    if (g.ctrl_pending == 0U) { g.ctrl_pending = ctrl; }   /* 重试 */
                    UNLOCK();
                }
            }
        }

        /* ---- 心跳：★ 两块板**都**发 ---- */
        if ((now - hb_tx_t0) >= APP_CAN_HB_MS)
        {
            hb_tx_t0 = now;
            (void)Can_SendHeartbeat((u8)BOARD_SEL);
        }

#if IS_MASTER

        /* ---- 2) 广播本板数据：新样本自动发 / 按键4 手动重发 ----
               ★★ 直接读 g 里的"最新样本快照"，**不再用 xQueuePeek(q_sample)**。
                  原因：q_sample 的消费者是 WifiTask（它按 HAL_GetTick 限速出队），
                  CanTask 只是 Peek 队头。一旦 WifiTask 没在出队（WiFi 未连、
                  任务被长时间占用、或时基异常），队列很快塞满 8 个，
                  **队头就永远是最老那个样本 → s.count 不再变 → CAN 广播永久停发**。
                  CAN 广播绝不该依赖"WiFi 有没有在跑"，所以改成直接取快照。 */
        {
            app_state_t s;
            u8 force;
            LOCK(); force = g.resend; g.resend = 0U; UNLOCK();

            LOCK(); s = g; UNLOCK();

            if (s.count != 0U)              /* 没开始采集（count==0）就不广播 */
            {
                static u32 last_bc = 0xFFFFFFFFU;
                if ((s.count != last_bc) || (force != 0U))
                {
                    if (Can_SendData(s.temp10, s.humi10, (u16)s.light,
                                     (u16)s.eco2, (u16)s.tvoc,
                                     (u16)s.count,
                                     (u8)((s.alarm != 0U) ? 0U : 1U)) != 0U)
                    {
                        last_bc = s.count;      /* ★ 发成功才记序号；失败下一拍补发 */
                        g_can_tx_cnt++;
                    }
                }
            }
        }

        /* ---- 3) 收：板2 心跳（判在线）+ 板2 的 CTRL（★ 板2 按键也能启停本板）---- */
        while (Can_Recv(&m) != 0U)
        {
            g_can_rx_cnt++;
            if ((m.id == CAN_ID_HEARTBEAT) && (m.dlc >= 1U))
            {
                if (m.data[0] == 2U) { hb_b2_t = now; }   /* 只认板2（实验七两块板）*/
            }
            else if ((m.id == CAN_ID_CTRL_B) && (m.dlc >= 1U))
            {
                /* ★ 两板等价：板2 按了 KEY0/KEY1，本板跟着一起启停。
                   send_ctrl=0 —— 绝不再转发一次，否则两板会互相刷控制帧。 */
                u8 run = (u8)((m.data[0] != 0U) ? 1U : 0U);
                App_SetRunning(run, 0U);
                UART_Printf("[CAN] <- CTRL %s (from b2)%c%c",
                            (run != 0U) ? "START" : "STOP", 13U, 10U);
            }
            /* ★ 题2：板2 回传的**原始 5 项**（0x110/0x111）→ 存进 peer_*，
               给 AiTask 判"这条数据来自板2"（交叉验证闭环）。 */
            else if ((m.id == CAN_ID_PEER_A) && (m.dlc >= 8U))
            {
                LOCK();
                g.peer_seq    = (u32)(((u32)m.data[0] << 8) | (u32)m.data[1]);
                g.peer_temp10 = (s32)(s16)(((u16)m.data[2] << 8) | (u16)m.data[3]);
                g.peer_humi10 = (s32)(s16)(((u16)m.data[4] << 8) | (u16)m.data[5]);
                g.peer_light  = (u32)(((u32)m.data[6] << 8) | (u32)m.data[7]);
                g.peer_have   = 1U;
                UNLOCK();
            }
            else if ((m.id == CAN_ID_PEER_B) && (m.dlc >= 6U))
            {
                LOCK();
                g.peer_eco2 = (u32)(((u32)m.data[0] << 8) | (u32)m.data[1]);
                g.peer_tvoc = (u32)(((u32)m.data[2] << 8) | (u32)m.data[3]);
                g.peer_ok   = (u8)((m.data[4] != 0U) ? 1U : 0U);
                g.peer_have = 1U;
                UNLOCK();
            }
        }
        LOCK();
        g.b2_alive = (u8)(((now - hb_b2_t) < APP_PEER_TIMEOUT_MS) ? 1U : 0U);
        UNLOCK();

#else
        /* ---- 从机：先把本板原始值**回传**给板1（★ 题2 的 AI 交叉验证要用）----
               与板1 的广播对称：count 变化就发一次；发成功才记序号，失败下一拍补发。 */
        {
            app_state_t s;
            LOCK(); s = g; UNLOCK();
            if (s.count != 0U)
            {
                static u32 last_peer = 0xFFFFFFFFU;
                if (s.count != last_peer)
                {
                    if (Can_SendPeer((s16)s.temp10, (s16)s.humi10, (u16)s.light,
                                     (u16)s.eco2, (u16)s.tvoc, (u16)s.count,
                                     (u8)((s.alarm != 0U) ? 0U : 1U)) != 0U)
                    {
                        last_peer = s.count;
                    }
                }
            }
        }

        /* ---- 从机：收板1 的数据/控制帧 ---- */
        while (Can_Recv(&m) != 0U)
        {
            g_can_rx_cnt++;

            if ((m.id == CAN_ID_DATA_A) && (m.dlc >= 8U))
            {
                LOCK();
                g.b1_seq    = (u32)(((u32)m.data[0] << 8) | (u32)m.data[1]);
                g.b1_temp10 = (s32)(s16)(((u16)m.data[2] << 8) | (u16)m.data[3]);
                g.b1_humi10 = (s32)(s16)(((u16)m.data[4] << 8) | (u16)m.data[5]);
                g.b1_light  = (u32)(((u32)m.data[6] << 8) | (u32)m.data[7]);
                g.b1_have   = 1U;
                UNLOCK();
                b1_last_t = now;
            }
            else if ((m.id == CAN_ID_DATA_B) && (m.dlc >= 6U))
            {
                LOCK();
                g.b1_eco2 = (u32)(((u32)m.data[0] << 8) | (u32)m.data[1]);
                g.b1_tvoc = (u32)(((u32)m.data[2] << 8) | (u32)m.data[3]);
                g.b1_ok   = (u8)((m.data[4] != 0U) ? 1U : 0U);
                g.b1_have = 1U;
                UNLOCK();
                b1_last_t = now;
            }
            else if ((m.id == CAN_ID_CTRL) && (m.dlc >= 1U))
            {
                /* ★ 两板等价：板1 按了 KEY0/KEY1 → 本板**自己也跟着启停**
                   （原先只置 b1_on 开关「板1区」；现在本板真的开始/停止采集）。
                   send_ctrl=0：不再转发，避免两板互相刷控制帧。 */
                u8 run = (u8)((m.data[0] != 0U) ? 1U : 0U);
                if (run == 0U)
                {
                    LOCK(); g.b1_have = 0U; UNLOCK();   /* STOP：清「板1区」，回到占位 */
                }
                App_SetRunning(run, 0U);
                UART_Printf("[CAN] <- CTRL %s (from b1)%c%c",
                            (run != 0U) ? "START" : "STOP", 13U, 10U);
            }
            /* 其它 ID（含板1 心跳 0x180）忽略 */
        }

        /* ---- 板1 在线判定：3s 内收不到数据帧 → 离线 ---- */
        LOCK();
        g.b1_alive = (u8)(((now - b1_last_t) < APP_PEER_TIMEOUT_MS) ? 1U : 0U);
        /* ★ 题2：把板1 广播来的原始值同步进 peer_*（板2 侧"对板数据"的来源），
           这样 AiTask 两板都能用同一套 peer_* 字段，不用分支。 */
        if (b1_last_t != 0U)
        {
            g.peer_seq    = g.b1_seq;
            g.peer_temp10 = g.b1_temp10;
            g.peer_humi10 = g.b1_humi10;
            g.peer_light  = g.b1_light;
            g.peer_eco2   = g.b1_eco2;
            g.peer_tvoc   = g.b1_tvoc;
            g.peer_ok     = g.b1_ok;
            g.peer_have   = g.b1_have;
        }
        UNLOCK();
#endif
    }
}

/*==============================================================================
 *  KeyTask：按键扫描（板上网络名 KEY0..KEY3，= 5 向按键的四个方向）
 *      板1：KEY0 = 开始采集 + CAN CTRL START；KEY1 = 停止 + CTRL STOP
 *            KEY2 = 切数据页/状态页；        KEY3 = 手动重发一次 CAN 快照
 *      板2：上电即采（不靠按键）；KEY2 = 切页；KEY3 = 手动恢复 I2C 总线
 *  独占：PF6~9
 *============================================================================*/
static void KeyTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    (void)arg;

    for (;;)
    {
        u8 k;

        TaskWaitPeriod(&last, pdMS_TO_TICKS(APP_KEY_SCAN_MS));

        key_scan(0U);
        k = keydown_data;
        if (k == 0U) { continue; }
        keydown_data = 0U;

        if (k == KEY0_DATA)                 /* KEY0(PF9)：开始采集（★ 两板等价，按哪块都行）*/
        {
            App_SetRunning(1U, 1U);         /* 本板开 → 并让 CanTask 通知对板一起开 */
            UART_Puts((const char *)TXT_KEY1);
        }
        else if (k == KEY1_DATA)            /* KEY1(PF8)：停止采集（★ 同上）*/
        {
            App_SetRunning(0U, 1U);
            UART_Puts((const char *)TXT_KEY2);
        }
        else if (k == KEY2_DATA)            /* ★ KEY2(PF7)：切换显示页（两板通用）*/
        {
            u8 pg;
            LOCK();
            g.page = (u8)((g.page == 0U) ? 1U : 0U);
            pg = g.page;
            UNLOCK();
            UART_Printf("[KEY] page -> %s%c%c", (pg != 0U) ? "STATUS" : "DATA", 13U, 10U);
        }
        else if (k == KEY3_DATA)            /* ★ KEY3(PF6)：手动动作（按角色不同）*/
        {
#if IS_MASTER
            LOCK(); g.resend = 1U; UNLOCK();
            UART_Printf("[KEY] force resend CAN snapshot%c%c", 13U, 10U);
#else
            LOCK(); g.i2c_recover = 1U; UNLOCK();
            UART_Printf("[KEY] request I2C bus recover%c%c", 13U, 10U);
#endif
        }
    }
}

/*==============================================================================
 *  WifiTask：推联网状态机 + 出队 → 组 D 帧 / S 帧上报 PC
 *  独占：UART4
 *============================================================================*/
static void WifiTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    u32 last_tx = 0U;
    u8  need_hello = 0U;
    u8  last_s_state = 0xFFU;           /* 上次已上报的采集状态（0xFF = 还没报过）*/
    (void)arg;

    for (;;)
    {
        TaskWaitPeriod(&last, pdMS_TO_TICKS(5U));

        Wifi_Tick();

        LOCK();
        g.wifi_ok = Wifi_LinkOk(WIFI_LINK_PC);
        if (g.wifi_ok != 0U)
        {
            if (g.greeted == 0U) { g.greeted = 1U; need_hello = 1U; }
        }
        else
        {
            g.greeted = 0U;
        }
        UNLOCK();

        if (need_hello != 0U)               /* 连上：发一次 H,姓名,学号,板号 */
        {
            u8  hs[48];
            u16 n = App_BuildHandshake(hs);
            if (Wifi_Push(hs, n, WIFI_LINK_PC) != 0U)
            {
                need_hello = 0U;
                last_s_state = 0xFFU;       /* ★ 重新握手 → S 帧也补发一次当前状态 */
                UART_Printf("[WiFi] handshake sent (H,name,id,b%u)%c%c",
                            (u32)BOARD_SEL, 13U, 10U);
            }
        }

        /* ★ v7：采集启停状态变化 → 立刻发一条 S 帧（题1：网页显示采集开始/停止）。
           判据是"和上次报过的值不同"，而不是"这一拍变了" —— 于是：
             · 链路没连上时 Push 返回 0 → 不更新 last_s_state → 连上后自动补发；
             · 中间漏报多次也只发最终状态，不会刷屏。
           板2 上电即采（running 恒 1），开机握手后就会发一条 S,2,START。 */
        {
            u8 run;
            LOCK(); run = g.running; UNLOCK();
            if (run != last_s_state)
            {
                u8  sb[24];
                u16 sn = App_BuildStatusFrame(sb, run);
                if (Wifi_Push(sb, sn, WIFI_LINK_PC) != 0U)
                {
                    last_s_state = run;
                    UART_Printf("[WiFi] S,%u,%s sent%c%c",
                                (u32)BOARD_SEL, (run != 0U) ? "START" : "STOP",
                                13U, 10U);
                }
            }
        }

        /* 出队样本 → 组 D 帧 → 发给 PC（限速）*/
        if ((HAL_GetTick() - last_tx) >= APP_TX_GAP_MS)
        {
            sample_t s;
            if (xQueueReceive(q_sample, &s, 0) == pdTRUE)
            {
                u8  buf[80];
                u8  acls, aconf;
                u16 n;

                /* ★ 题2：把本样本的 AI 判定一起发上去（未集成时为 0 → 上位机显示 --）*/
                LOCK(); acls = g.ai_own_cls; aconf = g.ai_own_conf; UNLOCK();
                n = App_BuildDataFrame(buf, s.up_ms, s.count,
                                       s.temp10, s.humi10, s.light,
                                       s.eco2, s.tvoc, s.alarm, acls, aconf);
                last_tx = HAL_GetTick();
                (void)Wifi_Push(buf, n, WIFI_LINK_PC);
            }
        }
    }
}

/*==============================================================================
 *  AiTask：题2 的神经网络推理（1Hz）
 *
 *  每次做两件事：
 *    ① 判**本板**刚采的这一条 → g.ai_own_cls/conf（随后随 D 帧上报 PC、屏上也显示）
 *    ② 判**对板**经 CAN 来的那一条 → g.ai_peer_cls/conf
 *       ★ 这就是题2 要的演示闭环：板1 拿 CAN 回传的板2 原始值，
 *         用**同一份模型**判出"这条数据来自板2"（板2 侧则判板1 广播来的数据）。
 *
 *  ★ 为什么单独建任务（而不是塞进 SensorTask）：
 *     推理有确定的最坏耗时（几毫秒），塞进 1Hz 的采集任务会把采样周期抖动放大；
 *     单独建任务可独立测量耗时、独立降频，也不与 I2C/LCD 抢时序。
 *  ★ 未集成模型时（Ai_Ready()==0）本任务什么都不干 —— 整个工程照常跑，
 *     上位机的 AI 两列显示 "--"。
 *  独占：无（只读 g + 调 Ai_Infer）
 *============================================================================*/
static void AiTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    (void)arg;

    for (;;)
    {
        app_state_t s;
        u8 cls;
        u8 conf;

        TaskWaitPeriod(&last, pdMS_TO_TICKS(APP_AI_MS));

        if (Ai_Ready() == 0U)               /* 模型没集成：直接跳过 */
        {
            continue;
        }

        LOCK(); s = g; UNLOCK();

        if (s.running == 0U)                /* 没在采集：清掉旧判定，免得上位机看到过期值 */
        {
            LOCK();
            g.ai_own_cls  = 0U; g.ai_own_conf  = 0U;
            g.ai_peer_cls = 0U; g.ai_peer_conf = 0U;
            UNLOCK();
            continue;
        }

        /* ---- ① 本板 ---- */
        if (Ai_Infer(s.temp10, s.humi10, s.light, s.eco2, s.tvoc,
                     &cls, &conf) != 0U)
        {
            LOCK(); g.ai_own_cls = cls; g.ai_own_conf = conf; UNLOCK();
        }

        /* ---- ② ★ 对板（CAN 来的原始值）—— 交叉验证 ---- */
        if (s.peer_have != 0U)
        {
            if (Ai_Infer(s.peer_temp10, s.peer_humi10, s.peer_light,
                         s.peer_eco2, s.peer_tvoc, &cls, &conf) != 0U)
            {
                LOCK(); g.ai_peer_cls = cls; g.ai_peer_conf = conf; UNLOCK();
            }
        }
    }
}

/*==============================================================================
 *  ★ 题2：AI 判定显示（两板共用，只是摆放的行不同）
 *      OWN  = 本板刚采的这一条判成哪块板
 *      PEER = 对板经 CAN 来的那一条判成哪块板（交叉验证）
 *  未集成模型时显示 "--"。用 App_BlankAscii 按固定宽度先擦（纪律见 LCD 注释）。
 *============================================================================*/
static void App_ShowAIPair(u16 x, u16 y)
{
    app_state_t s;
    u16 cx;

    LOCK(); s = g; UNLOCK();

    App_BlankAscii(x, y, (u16)(x + 150U));      /* 按最宽内容先擦 */

    LCD_DisplayString(x, y, ASC_SIZE, (u8 *)"OWN");
    cx = (u16)(x + 24U);
    if ((s.ai_ready == 0U) || (s.ai_own_cls == 0U))
    {
        LCD_DisplayString(cx, y, ASC_SIZE, (u8 *)"--");
    }
    else
    {
        LCD_DisplayChar(cx, y, 'b', ASC_SIZE);
        LCD_DisplayChar((u16)(cx + 8U), y, (char)('0' + (char)s.ai_own_cls), ASC_SIZE);
        App_ShowNum((u16)(cx + 20U), y, (u32)s.ai_own_conf, 3U, 0U);
        LCD_DisplayChar((u16)(cx + 40U), y, '%', ASC_SIZE);
    }

    cx = (u16)(x + 72U);
    LCD_DisplayString(cx, y, ASC_SIZE, (u8 *)"PEER");
    cx = (u16)(cx + 32U);
    if ((s.ai_ready == 0U) || (s.ai_peer_cls == 0U))
    {
        LCD_DisplayString(cx, y, ASC_SIZE, (u8 *)"--");
    }
    else
    {
        LCD_DisplayChar(cx, y, 'b', ASC_SIZE);
        LCD_DisplayChar((u16)(cx + 8U), y, (char)('0' + (char)s.ai_peer_cls), ASC_SIZE);
        App_ShowNum((u16)(cx + 20U), y, (u32)s.ai_peer_conf, 3U, 0U);
        LCD_DisplayChar((u16)(cx + 40U), y, '%', ASC_SIZE);
    }
}

/*==============================================================================
 *  DisplayTask：读 g_state → 刷 LCD
 *  独占：FSMC
 *============================================================================*/
/*==============================================================================
 *  ★ 状态页（按键3 切换）—— 把所有诊断信息画到屏上，**不依赖串口**
 *  数据页 = 5 项数值；状态页 = 各子系统健康度（WiFi / CAN / 两个传感器 / 在线）
 *  每帧整页重画：简单、不会留残影。
 *============================================================================*/
static void App_RefreshStatus(void)
{
    app_state_t s;
    u32 secs;
    u16 y;

    LOCK(); s = g; UNLOCK();
    secs = HAL_GetTick() / 1000U;

    BRUSH_COLOR = BLUE;
    BACK_COLOR  = WHITE;
    /* ★ 不在这里 LCD_Clear：清一次屏要写 240x320=76800 个点（约 8ms 忙写），
       每 200ms 清一次纯属浪费。换页时 DisplayTask 已经清过了，
       这里只要把每行**定宽重画**即可（数值用 App_ShowNum 先擦后画）。 */

    /* r0 标题 */
    App_CN2(6U, Y_ROW[0], H16_ZHUANG, H16_TAI);

    /* r1 WiFi：状态机名 + 是否连上 */
    LCD_DisplayString(6U, Y_ROW[1], ASC_SIZE, (u8 *)"WiFi");
    LCD_DisplayString(46U, Y_ROW[1], ASC_SIZE, (u8 *)Wifi_StateName());
    App_ShowOK(170U, Y_ROW[1], s.wifi_ok);

    /* r2 / r3 CAN 收发计数 */
    LCD_DisplayString(6U, Y_ROW[2], ASC_SIZE, (u8 *)"CAN TX");
    App_ShowNum(66U, Y_ROW[2], g_can_tx_cnt, 5U, 1U);
    LCD_DisplayString(6U, Y_ROW[3], ASC_SIZE, (u8 *)"CAN RX");
    App_ShowNum(66U, Y_ROW[3], g_can_rx_cnt, 5U, 1U);

    /* r4 SHT30 */
    LCD_DisplayString(6U, Y_ROW[4], ASC_SIZE, (u8 *)"SHT30");
    App_ShowOK(60U, Y_ROW[4], s.sensor_ok);
    LCD_DisplayString(110U, Y_ROW[4], ASC_SIZE, (u8 *)"fail");
    App_ShowNum(140U, Y_ROW[4], (u32)s.sht_fail, 3U, 1U);

    /* r5 SGP30：ok / 预热标志 */
    LCD_DisplayString(6U, Y_ROW[5], ASC_SIZE, (u8 *)"SGP30");
    App_ShowOK(60U, Y_ROW[5], s.sgp_ok);
    LCD_DisplayString(110U, Y_ROW[5], ASC_SIZE, (u8 *)"warm");
    App_ShowNum(146U, Y_ROW[5], (u32)((s.sgp_ready != 0U) ? 1U : 0U), 1U, 1U);

    /* r6 / r7 节点在线 + 本板采集开关 */
#if IS_MASTER
    LCD_DisplayString(6U, Y_ROW[6], ASC_SIZE, (u8 *)"SLAVE2");
    App_ShowOK(70U, Y_ROW[6], s.b2_alive);
    /* r7 原为 SLAVE3（实验六三板）；实验七改为显示**本板采集开关**，
       与题1"按键1 开始 / 按键2 停止"一一对应（1=采集中，0=已停止）。 */
    LCD_DisplayString(6U, Y_ROW[7], ASC_SIZE, (u8 *)"SAMPLE");
    App_ShowNum(70U, Y_ROW[7], (u32)s.running, 1U, 1U);
#else
    LCD_DisplayString(6U, Y_ROW[6], ASC_SIZE, (u8 *)"MASTER1");
    App_ShowOK(70U, Y_ROW[6], s.b1_alive);
    /* ★ 实验七两板等价：本板也显示**自己的采集开关**（原来显示 PEER ON）。
       「板1区」的开关状态在数据页的标题颜色上（灰=停 / 橙=未连 / 绿=刷数据中）。*/
    LCD_DisplayString(6U, Y_ROW[7], ASC_SIZE, (u8 *)"SAMPLE");
    App_ShowNum(70U, Y_ROW[7], (u32)s.running, 1U, 1U);
#endif

    /* r8 序号 */
    LCD_DisplayString(6U, Y_ROW[8], ASC_SIZE, (u8 *)"NO");
    App_ShowNum(30U, Y_ROW[8], s.count, 5U, 1U);

    /* r9 运行时长（秒）*/
    LCD_DisplayString(6U, Y_ROW[9], ASC_SIZE, (u8 *)"RUN");
    App_ShowNum(30U, Y_ROW[9], secs, 6U, 0U);
    LCD_DisplayChar(70U, Y_ROW[9], 's', ASC_SIZE);

    /* r10 本板号 */
    LCD_DisplayString(6U, Y_ROW[10], ASC_SIZE, (u8 *)"BOARD");
    LCD_DisplayChar(42U, Y_ROW[10], (char)('0' + BOARD_SEL), ASC_SIZE);

    /* r11 按键提示（用板上网络名 KEY0..KEY3：切页=KEY2、动作=KEY3）*/
    y = Y_ROW[11];
    LCD_DisplayString(6U, y, ASC_SIZE, (u8 *)"KEY2:page KEY3:act");
}

/*==============================================================================
 *  DisplayTask：按当前页刷屏（数据页 / 状态页）
 *  独占：FSMC
 *============================================================================*/
static void DisplayTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    u8 last_page = 0xFFU;
    (void)arg;

    App_DrawStatic();                   /* 先把数据页画出来 */

    for (;;)
    {
        u8 pg;
        TaskWaitPeriod(&last, pdMS_TO_TICKS(APP_DISPLAY_MS));

        LOCK(); pg = g.page; UNLOCK();

        if (pg != last_page)            /* ★ 换页：清屏重画，避免两种页互相留残影 */
        {
            last_page = pg;
            LCD_Clear(WHITE);
            if (pg == 0U) { App_DrawStatic(); }     /* 数据页：整体重画（含署名）*/
            else          { App_DrawId(); }         /* ★ 状态页：只需补画底部署名 */
        }

        if (pg == 0U) { App_RefreshDisplay(); }
        else          { App_RefreshStatus(); }
    }
}

/*==============================================================================
 *  LedTask：LED 效果驱动（**两板都是呼吸灯**，实现见 led_effect.c）
 *  开关：★ 两板等价 —— 都由**本板 running** 驱动
 *        （板2 的 running 是经 CAN CTRL 从对板同步来的，效果与板1 一致）
 *============================================================================*/
static void LedTask(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    u8 last_on = 0xFFU;
    (void)arg;

    for (;;)
    {
        u8 on;
        TaskWaitPeriod(&last, pdMS_TO_TICKS(20U));

        LOCK(); on = (u8)((g.running != 0U) ? 1U : 0U); UNLOCK();
        if (on != last_on)
        {
            LedFx_SetOn(on);
            last_on = on;
        }
        LedFx_Tick();
    }
}

/*==============================================================================
 *  串口命令台
 *============================================================================*/
static void App_DebugCmd(const char *cmd)
{
    char c = cmd[0];

    if (c == '?')
    {
        UART_Printf("[D] cmds: ?=help l=light t=sht30 g=sgp30 s=status w=wifi "
                    "c=can-test a=<C> alarm-thr n=ai-test10%c%c", 13U, 10U);
    }
    else if ((c == 'l') || (c == 'L'))
    {
        UART_Printf("[D] light raw=%u avg=%u pct=%u/100%c%c",
                    (u32)Light_ReadRaw(), (u32)Light_ReadAvg(),
                    (u32)Light_ReadPercent(), 13U, 10U);
    }
    else if ((c == 't') || (c == 'T'))
    {
        s32 t10 = 0, h10 = 0;
        if (SHT30_ReadTempHumi10(&t10, &h10) == HAL_OK)
        {
            UART_Printf("[D] sht30 temp=%d.%dC humi=%d.%d%%RH%c%c",
                        (s32)(t10 / 10), (s32)(((t10 < 0) ? -t10 : t10) % 10),
                        (s32)(h10 / 10), (s32)(h10 % 10), 13U, 10U);
        }
        else { UART_Printf("[D] sht30 read FAIL%c%c", 13U, 10U); }
    }
    else if ((c == 'g') || (c == 'G'))
    {
        u16 e = 0U, v = 0U;
        u8  gok = 0U;
        if (SGP30_StartMeasure() == HAL_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(SGP30_MEASURE_WAIT_MS));   /* 睡着等，不忙等 */
            gok = (u8)((SGP30_FetchIAQ(&e, &v) == HAL_OK) ? 1U : 0U);
        }
        if (gok != 0U)
        {
            UART_Printf("[D] sgp30 eCO2=%u ppm  TVOC=%u ppb%c%c", (u32)e, (u32)v, 13U, 10U);
        }
        else { UART_Printf("[D] sgp30 read FAIL%c%c", 13U, 10U); }
    }
    else if ((c == 'a') || (c == 'A'))
    {
        /* 设置报警阈值（℃）：`a 35` = 35℃；不带数字就只打印当前值。
           ★ 默认已是 35℃，正常采集**不需要敲**。这个命令留给两个场景：
             ① 演示「超温报警」：敲 `a 28` 立刻触发（室温本来就 28℃），演示完 `a 35`
             ② 环境特别热/冷时微调
           ★ 只有**板1** 的阈值有意义 —— ALM 判定包在采样任务的 #if IS_MASTER 里，
             板2 敲了不会改变任何东西（它从来不发 ALM）。 */
        s32 v = 0;
        u8  has = 0U;
        const char *p = &cmd[1];

        while (*p == ' ') { p++; }
        while ((*p >= '0') && (*p <= '9')) { v = (v * 10) + (s32)(*p - '0'); has = 1U; p++; }
        if (has != 0U)
        {
            LOCK(); g_alarm_thr_x10 = v * 10; UNLOCK();
        }
        UART_Printf("[D] alarm thr = %d C  (usage: a <C>)%c%c",
                    (s32)(g_alarm_thr_x10 / 10), 13U, 10U);
    }
    else if ((c == 'n') || (c == 'N'))
    {
        /* ★ 题2 的演示命令：把 tools/train_nn.py 生成的那 10 条「任取 10 条后续记录」
           （ai_test10.h —— 每板最后 5 条，**没参与训练**）逐条跑一遍，
           打印 判定 / 置信度 / 真值 / 对错。把结果抄进文档就是题2 G5 的截图素材。 */
        if (Ai_Ready() == 0U)
        {
            UART_Printf("[D] AI not integrated (%s)%c%c", Ai_Info(), 13U, 10U);
        }
        else
        {
            u8 i;
            u8 hit = 0U;

            UART_Printf("[D] ==== AI test on %u held-out records ====%c%c",
                        (u32)AI_TEST10_N, 13U, 10U);
            for (i = 0U; i < AI_TEST10_N; i++)
            {
                /* ai_test10.h 存的是**真实值**（温度 25.3℃），本工程内部是 ×10 */
                s32 t10  = (s32)(AI_TEST10[i][0] * 10.0f);
                s32 h10  = (s32)(AI_TEST10[i][1] * 10.0f);
                u8  truth = AI_TEST10_TRUE[i];
                u8  cls  = 0U;
                u8  conf = 0U;

                if (Ai_Infer(t10, h10, (u32)AI_TEST10[i][2], (u32)AI_TEST10[i][3],
                             (u32)AI_TEST10[i][4], &cls, &conf) != 0U)
                {
                    if (cls == truth) { hit++; }
                    UART_Printf("[D] #%-2u truth=b%u -> b%u  %u%%  %s%c%c",
                                (u32)(i + 1U), (u32)truth, (u32)cls, (u32)conf,
                                (cls == truth) ? "OK" : "WRONG", 13U, 10U);
                }
                else
                {
                    UART_Printf("[D] #%-2u infer FAIL%c%c", (u32)(i + 1U), 13U, 10U);
                }
            }
            UART_Printf("[D] ==== %u/%u correct ====%c%c",
                        (u32)hit, (u32)AI_TEST10_N, 13U, 10U);
        }
    }
    else if ((c == 's') || (c == 'S'))
    {
        app_state_t s;
        LOCK(); s = g; UNLOCK();
        UART_Printf("[D] b%u run=%u seq=%u t=%d.%d h=%d.%d eco2=%u tvoc=%u wifi=%s can_tx=%u can_rx=%u ai=%s%c%c",
                    (u32)BOARD_SEL, (u32)s.running, s.count,
                    (s32)(s.temp10 / 10), (s32)(((s.temp10 < 0) ? -s.temp10 : s.temp10) % 10),
                    (s32)(s.humi10 / 10), (s32)(s.humi10 % 10),
                    s.eco2, s.tvoc, Wifi_StateName(), g_can_tx_cnt, g_can_rx_cnt,
                    Ai_Info(), 13U, 10U);
        UART_Printf("[D] ai: own=b%u %u%%  peer=b%u %u%%  (peer_have=%u)%c%c",
                    (u32)s.ai_own_cls, (u32)s.ai_own_conf,
                    (u32)s.ai_peer_cls, (u32)s.ai_peer_conf,
                    (u32)s.peer_have, 13U, 10U);
    }
    else if ((c == 'w') || (c == 'W'))
    {
        UART_Printf("[D] wifi state=%s ready=%u err=%u%c%c",
                    Wifi_StateName(), (u32)Wifi_IsReady(), (u32)wifi_err, 13U, 10U);
        /* ★ 实验七：重连原因计数 —— 判断"为什么老断"看这一行：
             closed  = 收到 0,CLOSED（对端关连接/上位机重启）
             txdead  = CIPSEND 失败判死链（半开连接）
             wifilost= 热点掉了            stepfail = AT 步骤失败（模块没答话）
             deep    = 深断电（模块真的不答话 → 查 ESP 供电）
           哪一类涨得最多，就查哪一类。 */
        UART_Printf("[D] wifi why: closed=%u txdead=%u wifilost=%u stepfail=%u deep=%u%c%c",
                    Wifi_DbgWhy(0U), Wifi_DbgWhy(1U), Wifi_DbgWhy(2U),
                    Wifi_DbgWhy(3U), Wifi_DbgWhy(4U), 13U, 10U);
    }
    else if ((c == 'c') || (c == 'C'))
    {
        /* CAN 自测：发一帧心跳（板1/从机都能发，用于查总线通不通）*/
        u8 ok = Can_SendHeartbeat((u8)BOARD_SEL);
        UART_Printf("[D] can send heartbeat b%u -> %s%c%c",
                    (u32)BOARD_SEL, (ok != 0U) ? "OK" : "FAIL", 13U, 10U);
    }
    else
    {
        UART_Printf("[D] unknown cmd, press ? for help%c%c", 13U, 10U);
    }
}

static void DebugTask(void *arg)
{
    char line[48];
    (void)arg;

    for (;;)
    {
        if (UartDebug_GetLine(line, (u16)sizeof(line)) != 0U)
        {
            App_DebugCmd(line);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/*==============================================================================
 *  对外接口
 *============================================================================*/

/* ★ 启动里程碑（诊断）：把 App_Init 走到哪一步打到屏上第 2 行（x=6, y=30）。
 *   白屏 / 卡死时，屏上停在哪个标签就说明死在 App_Init 的哪一步之后：
 *       S1 = LCD_Init 完成（BOOT b<n> 也已画出）
 *       S2 = 身份串口打印完成
 *       S3 = Can_Init 完成        （CE = Can_Init 返回了错误码，不是卡死）
 *       S4 = LedFx_Init 完成      ★ 从机在此启动 TIM7 中断（板1 不启动）
 *       S5 = LedFx_SetOn + [LED] 日志完成
 *       S6 = 从机自动开始采集分支完成  ★ 仅从机执行那句 UART_Printf
 *       S7 = 7 个任务全部创建完成（随后被 "TASK OK" 覆盖）
 *   正常运行时会被 "TASK OK" / 数据页覆盖，所以只在异常时才可见。
 *   交付演示前把 BOOT_MARK_EN 改成 0 即可全部去掉。 */
#define BOOT_MARK_EN   1

#if BOOT_MARK_EN
/* 在第 2 行画一个 3 字符标签（不足补空格，天然擦掉上一拍更长的残留） */
static void Boot_Mark(const char *tag)
{
    u8 buf[4];
    u8 i;

    for (i = 0U; i < 3U; i++) { buf[i] = (u8)' '; }
    for (i = 0U; (i < 3U) && (tag[i] != '\0'); i++) { buf[i] = (u8)tag[i]; }
    buf[3] = 0U;
    LCD_DisplayString(6U, 30U, ASC_SIZE, buf);
}

/* ★ HAL 时基自检：用 DWT 忙等 20ms（**不依赖 HAL 时基**），看 HAL_GetTick() 涨不涨。
 *   屏上第 3 行显示 "TB <d>"，d = min(这 20ms 里 uwTick 的增量, 9)：
 *        TB 9 = 时基正常（20ms 至少涨了 9）
 *        TB 0 = **HAL 时基（TIM6）根本没在走** —— 所有 HAL 超时全部失效，
 *               任何靠 HAL_GetTick() 兜底的等待（HAL_CAN_Start / HAL_Delay）都会死循环。
 *   这是本工程从实验五起就存在的坑（见 common.c 里 delay_ms 的说明）。 */
static void Boot_TickCheck(void)
{
    u32 t0 = HAL_GetTick();
    u32 d;

    delay_ms(20U);
    d = HAL_GetTick() - t0;
    if (d > 9U) { d = 9U; }

    LCD_DisplayString(6U, 54U, ASC_SIZE, (u8 *)"TB  ");
    LCD_DisplayChar((u16)(6U + 3U * ASC_W), 54U, (char)('0' + (char)d), ASC_SIZE);
}
#else
#define Boot_Mark(tag)     ((void)0)
#define Boot_TickCheck()   ((void)0)
#endif

void App_Init(void)
{
    u8 n_fail;

    delay_init();                       /* DWT，给 delay_us 用 */

    UART_DebugInit();                   /* 先建串口 TX 互斥锁（多任务都会打日志）*/

    g_mtx    = xSemaphoreCreateMutex();
    q_sample = xQueueCreate(8U, (UBaseType_t)sizeof(sample_t));

    UART_Printf("[BOOT] exp7 board%u (FreeRTOS, 2-node)%c%c", (u32)BOARD_SEL, 13U, 10U);
    UART_Puts((const char *)TXT_BOOT6);
    UART_Puts((const char *)TXT_EOL);

    /* ---- 屏先上，上电立刻有东西看 ---- */
    LCD_BACK = 1;
    LCD_Init();

    /* ★ 启动里程碑（屏上打点，**不依赖串口**）：
       LCD_Init 之后先画 "BOOT b<n>"，建完任务再补一行 "TASK OK"。
       正常运行时这两行几十毫秒后就被 DisplayTask 的数据页整屏覆盖，看不见；
       万一日后白屏/卡死，屏上停在哪一行就说明死在哪个阶段 ——
         连 "BOOT b2" 都没有 → 死在 LCD_Init 之前/之内
         停在 "BOOT b2"       → 死在 LCD_Init 之后、xTaskCreate 之前
         停在 "TASK OK"       → 死在调度器启动后、DisplayTask 画第一帧之前
       （交付演示前可以删掉这两行，纯诊断用。）*/
    BRUSH_COLOR = BLUE;
    BACK_COLOR  = WHITE;
    LCD_DisplayString(6U, 6U, ASC_SIZE, (u8 *)"BOOT b");
    LCD_DisplayChar((u16)(6U + 7U * ASC_W), 6U, (char)('0' + BOARD_SEL), ASC_SIZE);
    Boot_Mark("S1");                    /* S1: LCD 可用 */
    Boot_TickCheck();                   /* 第 3 行 TB<n>：HAL 时基自检（9=正常，0=没在走）*/
    /* App_DrawStatic 由 DisplayTask 启动时画（避免 LCD 与任务并发）*/

    /* ---- 本板身份 ---- */
    UART_Puts((const char *)TXT_NAME_PRE);
    UART_PutsN(stu_name_gb, STU_NAME_GB_N);
    UART_Puts((const char *)TXT_ID_PRE);
    UART_Puts(STU_ID);
    UART_Puts((const char *)TXT_EOL);
    Boot_Mark("S2");                    /* S2: 串口身份打印完成 */

    /* ---- 传感器初始化已移到 SensorTask 里做 ----
       ★ 为什么不在这里（App_Init）做：App_Init 跑在**调度器启动之前**，
         任何 I2C 阻塞都会让整个系统起不来（LCD/按键/WiFi 全都没有）。
         放进任务后：启动立刻完成；万一 I2C 卡住，只卡 SensorTask，
         其余任务照跑、屏上能看出"传感器 错误"。 */

    /* ---- CAN ---- */
    if (Can_Init() == 0U)
    {
        UART_Printf("[CAN] init done (500kbps, PA11/PA12)%c%c", 13U, 10U);
        Boot_Mark("S3");                /* S3: CAN 初始化完成 */
    }
    else
    {
        UART_Printf("[CAN] init FAILED (filter)%c%c", 13U, 10U);
        Boot_Mark("CE");                /* CE: Can_Init 返回错误（不是卡死）*/
    }

    /* ---- LED 效果 ---- */
    LedFx_Init();
    Boot_Mark("S4");                    /* S4: LED 效果初始化完成（从机在此启动 TIM7）*/
    LedFx_SetOn(0U);                    /* 上电不亮，由按键 / CAN 驱动 */
    UART_Printf("[LED] fx init done (board%u)%c%c", (u32)BOARD_SEL, 13U, 10U);
    Boot_Mark("S5");                    /* S5: LED 状态设定 + 日志完成 */

    /* ★ 实验七：两板等价 —— **上电都不采**，等按任意一板的 KEY0 才一起开始。
       （实验六是"从机上电即采"；改成对称后，"按任意板 KEY0 同时开始采集"
         才有意义 —— 否则板2 在 KEY0 之前就已经在采了。）
       ★ 这里**不能**用 LOCK()/UNLOCK()：本函数跑在 vTaskStartScheduler() 之前，
         FreeRTOS 内核还没起来（pxCurrentTCB 为空），"调度器启动前做阻塞式取信号量"
         是明确不该有的用法（这次能跑通只是因为那把锁恰好没人持有）。
         此刻一个任务都还没建，没有任何并发需要保护 —— 直接赋值。 */
    g.running = 0U;
    g.msg     = 0U;
    g.ai_ready = Ai_Ready();            /* ★ 题2：模型是否已集成（屏上/串口据此显示）*/
    UART_Printf("[AI] %s%c%c", Ai_Info(), 13U, 10U);
    Boot_Mark("S6");                    /* S6: 采集初值设定完成 */

    /* ---- 建任务：优先级 按键 > 采集 > CAN/WiFi > LED/显示 > 调试 ----
       ★ 栈是按"最坏路径"给的：UART_Printf 会在**调用者栈**上开 192 字节的
         格式化缓冲，所以任何会打日志的任务都不能给太小（key/debug 都调到 320）。
       ★ 返回值必须检查：堆不够时 xTaskCreate 返回 pdFAIL，任务**静默不存在**
         （最典型的就是 DisplayTask 没建起来 → 屏永远停在 LCD_Init 的白屏）。*/
    n_fail = 0U;
    if (xTaskCreate(SensorTask,  "sensor", 320U, NULL, 4U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(KeyTask,     "key",    320U, NULL, 5U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(CanTask,     "can",    320U, NULL, 3U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(WifiTask,    "wifi",   512U, NULL, 3U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(AiTask,      "ai",     512U, NULL, 2U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(LedTask,     "led",    128U, NULL, 2U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(DisplayTask, "disp",   512U, NULL, 2U, NULL) != pdPASS) { n_fail++; }
    if (xTaskCreate(DebugTask,   "debug",  320U, NULL, 1U, NULL) != pdPASS) { n_fail++; }

    UART_Printf("[TASK] created 8 - fail = %u (heap left = %u B)%c%c",
                (u32)n_fail, (u32)xPortGetFreeHeapSize(), 13U, 10U);
    Boot_Mark("S7");                    /* S7: 7 个任务创建完成 */
    if (n_fail != 0U)
    {
        /* 建不起来就别往下走了：屏上留证据（里程碑行下面一行）*/
        LCD_DisplayString(6U, 30U, ASC_SIZE, (u8 *)"TASK FAIL");
        for (;;) { }                    /* 停在这，便于串口/屏上取证 */
    }
    LCD_DisplayString(6U, 30U, ASC_SIZE, (u8 *)"TASK OK");   /* 里程碑 2 */

    Wifi_Start();
}

/* FreeRTOS 版里主循环被 osKernelStart() 取代，这个函数没人会调；
   留个空壳，是为了 main.c 里那句 App_Loop() 还能链接通过。*/
void App_Loop(void)
{
}
