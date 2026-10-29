#include "light.h"
#include "adc.h"        /* hadc1、MX_ADC1_Init()（CubeMX 生成）*/

/*==============================================================================
 *  light.c  ---  光敏 AO（PC0 = ADC1_IN10）驱动（HAL 版）
 *
 *  单通道、单次、软件触发、轮询读取，不占用中断、不用 DMA。
 *
 *  ★ ADC 现在由 STM32CubeMX 管（这是"能用 CubeMX 的都用"改造的一部分）：
 *    - .ioc 里 ADC1 已配好：PCLK2/4 时钟、12 位、右对齐、扫描关、连续关、
 *      单通道 CH10、软件触发、采样时间 480 Cycles、NVIC 不使能。
 *    - 生成的配置在 Core/Src/adc.c 的 MX_ADC1_Init()，由 main.c 调用，
 *      一定会早于本模块的 Light_Init()/Light_Read*()。
 *    - ★ 要改 ADC 参数（含采样时间）请改 .ioc 重新生成，别在这里改。
 *
 *  本文件只干两件事：上电校准一次；之后每次读一个值（轮询）。
 *============================================================================*/

/* 一次转换的最长等待：480 周期采样 + 12 周期转换 @21MHz ≈ 24us，给足余量 */
#define LIGHT_TMO_MS   5U

/* ★ 本工程的 HAL 是裁剪版：stm32f4xx_hal_adc_ex.c 里**没有**
   HAL_ADCEx_Calibration_Start，CMSIS 头也没定义 ADC_CR2.CAL ——
   所以"校准"这唯一一处还得自己写寄存器（其余全部走 HAL）。
   RM0090：ADC_CR2.CAL 写 1 启动校准，硬件校准完自动清零。 */
#define LIGHT_ADC_CR2_CAL   (0x1UL << 2)

void Light_Init(void)
{
    volatile u32 d;

    /* 让 ADC 上电（ADON），空转一会儿等 tSTAB（≥3us）。
       用空转而不是 delay_us：不依赖 DWT 是否已初始化。 */
    ADC1->CR2 |= ADC_CR2_ADON;
    for (d = 0U; d < 2000U; d++)
    {
        /* 等 tSTAB */
    }

    /* 启动校准，等硬件把 CAL 清掉。
       ★ 必须有超时：ADC 时钟异常/外设没起来时，无界 while 会**死等**，
         整个任务卡在这里出不来（这是最容易发生"卡死"的地方之一）。 */
    {
        u32 tmo = 200000U;
        ADC1->CR2 |= LIGHT_ADC_CR2_CAL;
        while (((ADC1->CR2 & LIGHT_ADC_CR2_CAL) != 0U) && (tmo != 0U))
        {
            tmo--;
        }
        /* 超时了也不阻塞：继续跑，只是读数可能不准（上层能从数值看出异常）*/
    }
}

u16 Light_ReadRaw(void)
{
    u16 v;

    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
        return 0U;
    }
    if (HAL_ADC_PollForConversion(&hadc1, LIGHT_TMO_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&hadc1);
        return 0U;                                     /* 超时：给 0，上层看得出异常 */
    }
    v = (u16)(HAL_ADC_GetValue(&hadc1) & 0xFFFFU);
    (void)HAL_ADC_Stop(&hadc1);

#if (LIGHT_INVERT != 0U)
    v = (u16)(LIGHT_AO_MAX - v);                       /* 取反：越亮 -> 读数越大 */
#endif
    return v;
}

u16 Light_ReadAvg(void)
{
    u32 sum = 0U;
    u8  i;

    for (i = 0U; i < LIGHT_AVG_N; i++)
    {
        sum += Light_ReadRaw();
    }
    return (u16)(sum / LIGHT_AVG_N);
}

u8 Light_ReadPercent(void)
{
    u32 v = Light_ReadAvg();

    /* 四舍五入： (v*100 + 满量程/2) / 满量程 */
    return (u8)((v * 100U + (LIGHT_AO_MAX / 2U)) / LIGHT_AO_MAX);
}
