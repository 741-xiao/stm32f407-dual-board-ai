#include "led_effect.h"
#include "led.h"

/*==============================================================================
 *  led_effect.c  ---  LED 效果实现（★ 实验七：**两块板都是呼吸灯**）
 *
 *  ★ 相对实验六的变化：
 *      实验六是"板1 流水灯 / 板2·板3 呼吸灯"，由 BOARD_SEL 二选一编译。
 *      实验七题目要求「两板 LED 灯进行呼吸灯功能」——两板效果**必须一致**，
 *      所以流水灯那套实现**已删除**，只剩下呼吸灯一份，两板共用、不再分叉。
 *      （原流水灯代码见 git 历史 838497f 之前的 Lab6/firmware/USER/LED/led_effect.c）
 *
 *  为什么两板能放心共用一套（原流水灯那套的唯一优势是"纯 GPIO、不用定时器"）：
 *    TIM7 是**基本定时器**：无引脚、无复用、不与 FSMC(LCD) / CAN / UART 争资源，
 *    也不会和 HAL 的 1ms 时基冲突（时基在 TIM6，见 Core/Src/stm32f4xx_hal_timebase_tim.c）。
 *    板2 上实验六已实测跑通，板1 硬件完全相同，直接同样跑即可。
 *
 *  本文件是**LED 的唯一出口**：app 层不再直接写 LED0/1/2，一律经过本模块。
 *============================================================================*/

/*==============================================================================
 *  呼吸灯（TIM7 软件 PWM）
 *
 *  TIM7：PSC 84-1（84MHz -> 1MHz）、ARR 100-1（100us 更新 -> 10kHz 中断）。
 *  中断里：pwm_cnt 0..99 循环，与亮度等级 fx_level 比较 -> 直写三灯 GPIO。
 *    - PWM 频率 = 10kHz / 100 = 100Hz（人眼无闪烁感）
 *    - fx_level 由 LedFx_Tick 每 20ms 调 ±1：0->100 渐亮 2s，
 *      100->0 渐暗 2s，全周期约 4s。
 *
 *  为什么手写 TIM7、不走 CubeMX：
 *    TIM7 无引脚、无复用、纯中断，初始化只有下面十来行；写在这里
 *    既免了"为一次改动去点一遍 CubeMX"，也避免碰生成代码。
 *    中断优先级 6（低于 RTOS 的临界区级别）——中断内只翻转 GPIO，
 *    不调用任何 FreeRTOS API、不打印。
 *
 *  硬件约束：板载 LED0/1/2 在 **PE3/PE4/PG9，都没有定时器通道**，
 *  所以呼吸灯只能用软件 PWM —— 靠 TIM7 的 10kHz 更新中断逐级比较翻转 GPIO。
 *============================================================================*/

#define FX_PWM_LEVELS  100U     /* 亮度级数（=PWM 周期内的中断格数）*/
#define FX_LVL_STEP_MS 20U      /* 亮度刷新步进：20ms 一级 */

/* 中断与任务共享（volatile；u16 读写在本 MCU 上原子）*/
static volatile u16 fx_pwm_cnt;
static volatile u16 fx_level;   /* 0..100：PWM 周期前 fx_level 格点亮 */
static volatile u8  fx_on;

static u16 fx_dir;              /* 1=渐亮 0=渐暗 */
static u32 fx_t0;

/*------------------------------------------------------------------------------
 *  TIM7 更新中断：软件 PWM 本体
 *----------------------------------------------------------------------------*/
void TIM7_IRQHandler(void)
{
    if ((TIM7->SR & TIM_SR_UIF) != 0U)
    {
        TIM7->SR = (u16)~TIM_SR_UIF;

        fx_pwm_cnt++;
        if (fx_pwm_cnt >= FX_PWM_LEVELS)
        {
            fx_pwm_cnt = 0U;
        }

        if ((fx_on != 0U) && (fx_pwm_cnt < fx_level))
        {
            LED0 = 0; LED1 = 0; LED2 = 0;       /* 低电平点亮 */
        }
        else
        {
            LED0 = 1; LED1 = 1; LED2 = 1;       /* 全灭（关效果时也走这里）*/
        }
    }
}

static void tim7_start(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;

    TIM7->PSC  = (u16)(84U - 1U);           /* 84MHz -> 1MHz */
    TIM7->ARR  = (u16)(FX_PWM_LEVELS - 1U); /* 100us -> 10kHz 更新中断 */
    TIM7->EGR  = TIM_EGR_UG;                /* 立即把 PSC 装进分频器 */
    TIM7->SR   = 0U;                        /* 清 UG 顺带产生的更新标志 */
    TIM7->DIER |= TIM_DIER_UIE;
    NVIC_SetPriority(TIM7_IRQn, 6U);
    NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 |= TIM_CR1_CEN;
}

void LedFx_Init(void)
{
    fx_pwm_cnt = 0U;
    fx_level   = 0U;
    fx_on      = 0U;
    fx_dir     = 1U;
    LED0 = 1; LED1 = 1; LED2 = 1;           /* 全灭 */

    tim7_start();                           /* 中断先跑起来；level=0 时输出全灭 */
}

void LedFx_SetOn(u8 on)
{
    if (on != 0U)
    {
        fx_level = 0U;
        fx_dir   = 1U;
        fx_t0    = HAL_GetTick();
        fx_on    = 1U;                      /* 从暗处开始渐亮 */
    }
    else
    {
        fx_on    = 0U;                      /* 中断里随即输出全灭 */
        fx_level = 0U;
    }
}

void LedFx_Tick(void)
{
    u32 now = HAL_GetTick();

    if (fx_on == 0U)
    {
        return;
    }
    if ((now - fx_t0) >= FX_LVL_STEP_MS)
    {
        fx_t0 += FX_LVL_STEP_MS;
        if ((now - fx_t0) >= (FX_LVL_STEP_MS * 8U))
        {
            fx_t0 = now;                    /* 落后太多：直接对齐 */
        }

        if (fx_dir != 0U)
        {
            if (fx_level < FX_PWM_LEVELS) { fx_level++; }
            if (fx_level >= FX_PWM_LEVELS) { fx_dir = 0U; }    /* 到顶转暗 */
        }
        else
        {
            if (fx_level > 0U) { fx_level--; }
            if (fx_level == 0U) { fx_dir = 1U; }               /* 到底转亮 */
        }
    }
}
