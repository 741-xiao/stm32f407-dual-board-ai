#include "can_link.h"

/*==============================================================================
 *  can_link.c —— CAN1 板间通信驱动（基于 CubeMX 生成的 HAL CAN）
 *
 *  ★ 依赖 CubeMX 生成物：
 *      - Core/Src/can.c 里的 MX_CAN1_Init()（由 main.c 调用，配好 CAN1 与 PA11/PA12）
 *      - Core/Inc/can.h 里的 hcan1
 *      - hal_conf.h 里 HAL_CAN_MODULE_ENABLED 打开、Drivers 里含 stm32f4xx_hal_can.c/.h
 *    本文件只做：**配置接收滤波器 + 启动 CAN + 收发封装**。
 *
 *  物理层：CAN1 = PA11(CAN1_RX)/PA12(CAN1_TX, AF9)；速率 500kbps（见教程）。
 *          ★ 不用 PD0/PD1 —— 那两个脚已被 FSMC 占用（LCD 数据线 D2/D3）。
 *          需接 CAN 收发器（TJA1050 / SN65HVD230）；三节点共挂一对差分线，
 *          两端各 120Ω 终端电阻，且**必须共地**。
 *          接线交叉：PA11(CAN1_RX) → 收发器 TXD；PA12(CAN1_TX) → 收发器 RXD。
 *  收发策略：FIFO0 + **轮询**（不加中断），由 CanTask 每 10ms 调 Can_Recv() 取帧；
 *            1Hz 的数据率下轮询绰绰有余，也避免动到中断向量表。
 *============================================================================*/

extern CAN_HandleTypeDef hcan1;         /* 由 Core/Src/can.c 提供 */

u8 Can_Init(void)
{
    CAN_FilterTypeDef f;

    /* 接收滤波器：0 号 —— 32 位、掩码模式、掩码全 0 = 接收所有报文 → FIFO0。
     * 三节点小网络，收下来由软件按 ID 分类即可，不必逐 ID 配滤波器。 */
    f.FilterIdHigh         = 0x0000U;
    f.FilterIdLow          = 0x0000U;
    f.FilterMaskIdHigh     = 0x0000U;
    f.FilterMaskIdLow      = 0x0000U;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterBank           = 0U;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterActivation     = CAN_FILTER_ENABLE;
    f.SlaveStartFilterBank = 14U;       /* 仅双 CAN 时用；单 CAN 无影响 */

    if (HAL_CAN_ConfigFilter(&hcan1, &f) != HAL_OK)
    {
        return 1U;                      /* 滤波器配置失败（本函数不会阻塞） */
    }

    /*==========================================================================
     *  ★★ 这里**故意不用 HAL_CAN_Start()** —— 这是板2 上板白屏的根因所在。
     *
     *  HAL_CAN_Start() 的实际动作是：
     *      CLEAR_BIT(MCR, INRQ);                       // 请求退出初始化模式
     *      while ((MSR & INAK) != 0U)                  // 等硬件确认
     *          if ((HAL_GetTick() - t0) > 10) return;  // ← 唯一的逃生出口
     *
     *  INAK 要清零，需要 CAN 硬件**同步到总线**（检测到 11 个连续隐性位）。
     *  而本工程把 HAL 的 1ms 时基从 SysTick 挪到了 TIM6（SysTick 让给 FreeRTOS，
     *  见 FreeRTOSConfig.h 的 `#define xPortSysTickHandler SysTick_Handler`），
     *  `common.c` 里也记录了实测结论"uwTick 不涨"。**时基一旦不涨，
     *  上面那个超时判断永远不会成立 → 死循环 → 主流程再也走不动**，
     *  现象就是"背光亮、屏上停在启动里程碑、串口再无输出"。
     *
     *  正确做法：清 INRQ 之后**不等**。硬件同步上总线后会自动进入 Normal 模式；
     *  未同步时发送会被硬件暂缓/报错，由上层（CanTask 每 10ms 一拍）自然重试。
     *  这样 CAN 启动不再依赖任何时基，启动流程也绝不会卡在这里。
     *==========================================================================*/
    CLEAR_BIT(hcan1.Instance->MCR, CAN_MCR_INRQ);
    hcan1.State     = HAL_CAN_STATE_LISTENING;   /* 与 HAL 的状态机保持一致 */
    hcan1.ErrorCode = HAL_CAN_ERROR_NONE;

    return 0U;
}

u8 Can_SendStd(u16 id, const u8 *data, u8 dlc)
{
    CAN_TxHeaderTypeDef tx;
    u32 mbx;

    if (dlc > 8U) { dlc = 8U; }

    /* ★ 非阻塞：发送邮箱满就先返回 0，由调用方（CanTask）下一拍再重试 ——
       绝不在这里 HAL_GetTick() 自旋等邮箱。有 FreeRTOS 就该让出 CPU。
       1Hz 的数据率下邮箱几乎不会满；万一满，下一拍补发即可。 */
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0U)
    {
        return 0U;
    }

    tx.StdId              = id;
    tx.ExtId              = 0U;
    tx.IDE                = CAN_ID_STD;
    tx.RTR                = CAN_RTR_DATA;
    tx.DLC                = dlc;
    tx.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(&hcan1, &tx, (uint8_t *)data, &mbx) != HAL_OK)
    {
        return 0U;
    }
    return 1U;
}

u8 Can_Recv(can_msg_t *out)
{
    CAN_RxHeaderTypeDef rx;

    if (out == NULL) { return 0U; }

    if (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0U) { return 0U; }

    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx, out->data) != HAL_OK)
    {
        return 0U;
    }
    out->id  = (u16)rx.StdId;
    out->dlc = (u8)rx.DLC;
    return 1U;
}

/*------------------------------------------------------------------------------
 *  板1 便捷封装
 *------------------------------------------------------------------------------*/
/* 把一次采样打成两帧（A=seq/t/h/light，B=eco2/tvoc/status/board）发出去。
   idA/idB 决定方向：板1 用 DATA_A/DATA_B（0x100/0x101）广播给板2；
   板2 用 PEER_A/PEER_B（0x110/0x111）**回传**给板1（题2 要拿对板原始值做 AI 判定）。*/
static u8 can_send_pair(u16 idA, u16 idB,
                        s16 t10, s16 h10, u16 light,
                        u16 eco2, u16 tvoc, u16 seq, u8 ok)
{
    u8 a[8];
    u8 b[8];
    u8 oka;
    u8 okb;

    /* A: seq(2) t10(2) h10(2) light(2) */
    a[0] = (u8)(seq >> 8);
    a[1] = (u8)(seq & 0xFFU);
    a[2] = (u8)(((u16)t10) >> 8);
    a[3] = (u8)(((u16)t10) & 0xFFU);
    a[4] = (u8)(((u16)h10) >> 8);
    a[5] = (u8)(((u16)h10) & 0xFFU);
    a[6] = (u8)(light >> 8);
    a[7] = (u8)(light & 0xFFU);

    /* B: eco2(2) tvoc(2) status(1) board(1) rsv(2) */
    b[0] = (u8)(eco2 >> 8);
    b[1] = (u8)(eco2 & 0xFFU);
    b[2] = (u8)(tvoc >> 8);
    b[3] = (u8)(tvoc & 0xFFU);
    b[4] = (ok != 0U) ? 0x01U : 0x00U;
    b[5] = (u8)BOARD_SEL;
    b[6] = 0U;
    b[7] = 0U;

    oka = Can_SendStd(idA, a, 8U);
    okb = Can_SendStd(idB, b, 8U);
    return (u8)(((oka != 0U) && (okb != 0U)) ? 1U : 0U);
}

u8 Can_SendData(s16 t10, s16 h10, u16 light, u16 eco2, u16 tvoc, u16 seq, u8 ok)
{
    return can_send_pair(CAN_ID_DATA_A, CAN_ID_DATA_B,
                         t10, h10, light, eco2, tvoc, seq, ok);
}

/* ★ 题2：板2 → 板1 回传本板原始 5 项（载荷与 DATA_A/DATA_B 完全一样，
   只是 ID 不同；板1 收到后交给 AI 判"这条数据来自板2"）。*/
u8 Can_SendPeer(s16 t10, s16 h10, u16 light, u16 eco2, u16 tvoc, u16 seq, u8 ok)
{
    return can_send_pair(CAN_ID_PEER_A, CAN_ID_PEER_B,
                         t10, h10, light, eco2, tvoc, seq, ok);
}

/* ★ 实验七：按"发送方是谁"选 ID —— 板1 用 0x200、板2 用 0x201。
   接收方认 ID 即可知道这是对板发来的启停命令（见 CanTask）。 */
u8 Can_SendCtrl(u8 board, u8 start)
{
    u8 d[1];
    d[0] = (start != 0U) ? 0x01U : 0x00U;
    return Can_SendStd((board == 1U) ? CAN_ID_CTRL : CAN_ID_CTRL_B, d, 1U);
}

u8 Can_SendHeartbeat(u8 board)
{
    u8 d[1];
    d[0] = board;
    return Can_SendStd(CAN_ID_HEARTBEAT, d, 1U);
}
