#ifndef __LED_H
#define __LED_H

#include "common.h"

/*==============================================================================
 *  板载 LED（低电平点亮）
 *  初始化已由 CubeMX 的 MX_GPIO_Init() 完成：
 *      PE3 / PE4 / PG9  ->  推挽输出，初值高电平（熄灭）
 *  因此这里不再提供 LED_Init()，只保留位带操作宏。
 *============================================================================*/
#define LED0    PEout(3)
#define LED1    PEout(4)
#define LED2    PGout(9)

#endif
