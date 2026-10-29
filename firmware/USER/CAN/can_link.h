#ifndef __CAN_LINK_H
#define __CAN_LINK_H

#include "common.h"

/*==============================================================================
 *  CAN 板间通信（STM32F407 内置 bxCAN1）
 *
 *  ★ 引脚选择（重要）：
 *      F407 的 CAN1 可用 PA11/PA12、PB8/PB9、PD0/PD1（都是 AF9 复用）。
 *      本工程 **PD0/PD1 已被 FSMC 占用**（fsmc.c：PD0=FSMC_D2、PD1=FSMC_D3，
 *      是 LCD 的数据线！），**不能选**。
 *      本工程采用 **PA11 = CAN1_RX、PA12 = CAN1_TX** —— 这是 CubeMX 的自动
 *      分配，也与常见 F407 教学板（板载 TJA1050 收发器接 PA11/PA12）一致。
 *      （若某些板子的 CAN 引到 PB8/PB9，改选那两个脚即可，驱动无需改动。）
 *      接线是**交叉**的：PA11(CAN1_RX) → 收发器 TXD；PA12(CAN1_TX) → 收发器 RXD。
 *
 *  物理层：需接 CAN 收发器（TJA1050 / SN65HVD230）；三节点共挂一对差分线，
 *          两端各 120Ω 终端电阻，且**必须共地**。
 *  速率：500 kbps（APB1 = 42MHz，Prescaler=6 → 7MHz，每比特 14 tq，采样点 78.6%）
 *
 *  依赖：CubeMX 生成的 Core/Src/can.c（MX_CAN1_Init / hcan1）与 HAL CAN 库。
 *        本驱动只做"滤波器 + 启动 + 收发封装"。
 *============================================================================*/

/* 报文 ID（标准帧，11 位） */
#define CAN_ID_DATA_A     0x100U    /* seq(2) t10(2) h10(2) light(2) */
#define CAN_ID_DATA_B     0x101U    /* eco2(2) tvoc(2) status(1) board(1) rsv(2) */
#define CAN_ID_HEARTBEAT  0x180U    /* board(1) —— 每块板都发心跳 */
/* ★ 实验七：控制帧**双向**（两板等价，按任意一板的 KEY0/KEY1 都能同时启停）
     —— 用两个 ID 区分"谁发的"，接收方按 ID 就知道这条该不该自己执行：
        0x200 = 板1 发 → 板2 收
        0x201 = 板2 发 → 板1 收
     不用一个 ID + 载荷里带发送者是出于兼容：0x200 的载荷保持 1 字节不变，
     实验六那套"板1 单向控制"的抓包/工具还能直接读。 */
#define CAN_ID_CTRL       0x200U    /* ctrl(1)：0x01=START 0x00=STOP（板1 → 板2）*/
#define CAN_ID_CTRL_B     0x201U    /* ctrl(1)：同上，但方向相反（板2 → 板1）*/

/* ★ 题2：对板数据回传（板2 → 板1）—— 载荷与 DATA_A/DATA_B 完全一样，只是 ID 不同。
   用途：AI 交叉验证闭环 —— 板1 拿板2 的**原始 5 项**，用同一份模型判"这条来自板2"。
   为什么另开 ID 而不复用 0x100/0x101：方向不同、接收方的处理也不同
   （板1 收 0x110/0x111 只存不显示；板2 收 0x100/0x101 要更新「板1 区」）。 */
#define CAN_ID_PEER_A     0x110U    /* seq(2) t10(2) h10(2) light(2) */
#define CAN_ID_PEER_B     0x111U    /* eco2(2) tvoc(2) status(1) board(1) rsv(2) */

typedef struct
{
    u16 id;             /* 标准帧 ID */
    u8  dlc;            /* 数据长度 0..8 */
    u8  data[8];        /* 载荷（字节序由上层约定） */
} can_msg_t;

/*==============================================================================
 *  初始化 CAN1（PA11/PA12, 500kbps, 收全部报文 → FIFO0）
 *
 *  ★ 返回 0 = 成功；非 0 = 失败（1 = 滤波器配置失败）。
 *  ★ 内部**不调用 HAL_CAN_Start()** —— 那个函数 while 等 INAK 清零，
 *    唯一出口是 HAL_GetTick() 超时；本工程的 HAL 时基（TIM6）一旦不涨，
 *    就是**死循环**（板2 上板白屏卡在启动期的现场根因）。
 *    本驱动改为直接清 MCR.INRQ 退初始化模式，硬件同步上总线后自动进 Normal。
 *============================================================================*/
u8 Can_Init(void);

/* 发送一帧标准帧；成功返回 1，邮箱满/超时返回 0 */
u8 Can_SendStd(u16 id, const u8 *data, u8 dlc);

/* 非阻塞取一帧（从 FIFO0）；取到返回 1，空返回 0 */
u8 Can_Recv(can_msg_t *out);

/*==============================================================================
 *  便捷封装：数据 / 控制 / 心跳
 *============================================================================*/
u8 Can_SendData(s16 t10, s16 h10, u16 light, u16 eco2, u16 tvoc, u16 seq, u8 ok);

/* ★ 题2：板2 → 板1 回传本板原始 5 项（0x110/0x111）。载荷格式与 Can_SendData 相同。*/
u8 Can_SendPeer(s16 t10, s16 h10, u16 light, u16 eco2, u16 tvoc, u16 seq, u8 ok);

/* 控制帧（1=START 0=STOP）。★ 实验七：**board 决定用哪个 ID 发**
     board=1 → 0x200（板1 → 板2）；board=2 → 0x201（板2 → 板1）。
   两板等价：按任意一板的 KEY0/KEY1 都能让两板同时启停。 */
u8 Can_SendCtrl(u8 board, u8 start);

/* 心跳帧（每块板都发自己的板号） */
u8 Can_SendHeartbeat(u8 board);

#endif
