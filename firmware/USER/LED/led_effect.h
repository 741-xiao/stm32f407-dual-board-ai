#ifndef __LED_EFFECT_H
#define __LED_EFFECT_H

#include "common.h"

/*==============================================================================
 *  led_effect  ---  板载 LED 效果驱动（实验五新增）
 *
 *  角色分叉（BOARD_SEL），两块板共用本模块、效果不同：
 *      板1 -> 流水灯：LED0 -> LED1 -> LED2 单向轮转，300ms 一步
 *      板2 -> 呼吸灯：三灯同步渐亮渐暗（TIM7 软件 PWM，100 级 @100Hz）
 *
 *  硬件约束：板载 LED0/1/2 在 PE3/PE4/PG9，**都没有定时器通道**，
 *  所以呼吸灯只能用软件 PWM —— 用 TIM7 的 10kHz 更新中断逐级比较翻转 GPIO。
 *  TIM7 无引脚、纯中断，初始化直接写寄存器（与"光敏校准"同属例外，
 *  不进 CubeMX、不动生成代码）；中断服务函数 TIM7_IRQHandler 就放在
 *  led_effect.c 里（启动文件的向量表按名字引用，不需要声明）。
 *
 *  调用方式（app 侧，LedTask 每 20ms 调一次 LedFx_Tick）：
 *      LedFx_Init();            // App_Init 里调一次
 *      LedFx_SetOn(1);          // 开效果（角色决定：板1 流水 / 板2 呼吸）
 *      ... LedFx_Tick(); ...    // 周期任务里 20ms 一次
 *      LedFx_SetOn(0);          // 停效果、三灯全灭
 *============================================================================*/

void LedFx_Init(void);
void LedFx_SetOn(u8 on);
void LedFx_Tick(void);

#endif
