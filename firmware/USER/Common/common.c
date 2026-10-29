#include "common.h"

/*==============================================================================
 *  延时实现说明
 *
 *  实验2 用 SysTick->VAL 手搓微秒延时，并且在 delay_init() 里把 SysTick
 *  时钟源改成 HCLK。本工程改成 CubeMX + HAL，SysTick 已经被 HAL 用作 1ms
 *  时基，再动它会破坏 HAL_Delay。因此这里改用 Cortex-M4 内核自带的
 *  DWT 周期计数器做微秒延时，完全不碰 SysTick。
 *
 *  DWT->CYCCNT 每个 CPU 周期加 1，168MHz 下 1us = 168 个计数。
 *============================================================================*/

void delay_init(void)
{
    /* 使能 DWT 跟踪单元 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    /* 计数器清零并使能 */
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
 * @brief  微秒级延时（忙等）
 * @param  nus  延时的微秒数
 */
void delay_us(u32 nus)
{
    u32 start;
    u32 ticks;

    if (nus == 0U)
    {
        return;
    }

    start = DWT->CYCCNT;
    ticks = nus * (SystemCoreClock / 1000000U);

    /* 无符号减法天然支持计数器回绕 */
    while ((DWT->CYCCNT - start) < ticks)
    {
    }
}

/**
 * @brief  毫秒级延时（DWT 忙等，**不依赖 HAL 时基**）
 * @param  nms  延时的毫秒数
 *
 * ★ 为什么不用 HAL_Delay：
 *   本工程（FreeRTOS 版）把 HAL 的 1ms 时基从 SysTick 挪到了 TIM6（SysTick 让给
 *   FreeRTOS）。实测 `HAL_Delay()` 在这里会**一直等下去**（uwTick 不涨），于是
 *   `LCD_Init()` 里那句 `delay_ms(120)` 卡死 —— 现象就是**背光已亮、画面出不来（白屏）**。
 *   这里改用 DWT 周期计数器忙等（和 `delay_us` 一个套路），**不依赖任何时基**；
 *   而且调度器启动前也能用（`LCD_Init()` 正好在这个阶段调）。
 */
void delay_ms(u16 nms)
{
    while (nms != 0U)
    {
        delay_us(1000U);
        nms--;
    }
}
