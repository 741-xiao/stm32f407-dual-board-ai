#ifndef __COMMON_H
#define __COMMON_H

#include "stm32f4xx_hal.h"

/*==============================================================================
 *  板角色：**同一个 Keil 工程、两个 Target** 编出板1 / 板2。
 *  Target 的 C/C++ Define 里指定：board1 -> BOARD_SEL=1，board2 -> BOARD_SEL=2。
 *  没定义时默认 1（板1）—— 单独编某个文件、或 IDE 里预览也不会出错。
 *  只在"角色确实不同"的少数几处用 #if (BOARD_SEL == 2)。
 *============================================================================*/
#ifndef BOARD_SEL
#define BOARD_SEL 1
#endif

/*==============================================================================
 *  数据类型简写
 *  沿用实验2 / 正点原子的命名习惯，便于与既有驱动代码保持一致。
 *============================================================================*/
typedef int32_t  s32;
typedef int16_t  s16;
typedef int8_t   s8;

typedef const int32_t sc32;
typedef const int16_t sc16;
typedef const int8_t  sc8;

typedef __IO int32_t  vs32;
typedef __IO int16_t  vs16;
typedef __IO int8_t   vs8;

typedef __I int32_t vsc32;
typedef __I int16_t vsc16;
typedef __I int8_t  vsc8;

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef const uint32_t uc32;
typedef const uint16_t uc16;
typedef const uint8_t  uc8;

typedef __IO uint32_t vu32;
typedef __IO uint16_t vu16;
typedef __IO uint8_t  vu8;

typedef __I uint32_t vuc32;
typedef __I uint16_t vuc16;
typedef __I uint8_t  vuc8;

/*==============================================================================
 *  位带操作：像 51 那样直接读写单个 GPIO 位
 *  用法：LED0 = 1;  if (KEY0 == 0) ...
 *  注意 n 必须小于 16
 *============================================================================*/
#define BITBAND(addr, bitnum) ((addr & 0xF0000000) + 0x2000000 + ((addr & 0xFFFFF) << 5) + (bitnum << 2))
#define MEM_ADDR(addr)        *((volatile unsigned long *)(addr))
#define BIT_ADDR(addr, bitnum) MEM_ADDR(BITBAND(addr, bitnum))

/* IO 口地址映射 */
#define GPIOA_ODR_Addr    (GPIOA_BASE + 20)     /* 0x40020014 */
#define GPIOB_ODR_Addr    (GPIOB_BASE + 20)     /* 0x40020414 */
#define GPIOC_ODR_Addr    (GPIOC_BASE + 20)     /* 0x40020814 */
#define GPIOD_ODR_Addr    (GPIOD_BASE + 20)     /* 0x40020C14 */
#define GPIOE_ODR_Addr    (GPIOE_BASE + 20)     /* 0x40021014 */
#define GPIOF_ODR_Addr    (GPIOF_BASE + 20)     /* 0x40021414 */
#define GPIOG_ODR_Addr    (GPIOG_BASE + 20)     /* 0x40021814 */
#define GPIOH_ODR_Addr    (GPIOH_BASE + 20)     /* 0x40021C14 */
#define GPIOI_ODR_Addr    (GPIOI_BASE + 20)     /* 0x40022014 */

#define GPIOA_IDR_Addr    (GPIOA_BASE + 16)     /* 0x40020010 */
#define GPIOB_IDR_Addr    (GPIOB_BASE + 16)     /* 0x40020410 */
#define GPIOC_IDR_Addr    (GPIOC_BASE + 16)     /* 0x40020810 */
#define GPIOD_IDR_Addr    (GPIOD_BASE + 16)     /* 0x40020C10 */
#define GPIOE_IDR_Addr    (GPIOE_BASE + 16)     /* 0x40021010 */
#define GPIOF_IDR_Addr    (GPIOF_BASE + 16)     /* 0x40021410 */
#define GPIOG_IDR_Addr    (GPIOG_BASE + 16)     /* 0x40021810 */
#define GPIOH_IDR_Addr    (GPIOH_BASE + 16)     /* 0x40021C10 */
#define GPIOI_IDR_Addr    (GPIOI_BASE + 16)     /* 0x40022010 */

/* 输出 */
#define PAout(n)  BIT_ADDR(GPIOA_ODR_Addr, n)
#define PBout(n)  BIT_ADDR(GPIOB_ODR_Addr, n)
#define PCout(n)  BIT_ADDR(GPIOC_ODR_Addr, n)
#define PDout(n)  BIT_ADDR(GPIOD_ODR_Addr, n)
#define PEout(n)  BIT_ADDR(GPIOE_ODR_Addr, n)
#define PFout(n)  BIT_ADDR(GPIOF_ODR_Addr, n)
#define PGout(n)  BIT_ADDR(GPIOG_ODR_Addr, n)
#define PHout(n)  BIT_ADDR(GPIOH_ODR_Addr, n)
#define PIout(n)  BIT_ADDR(GPIOI_ODR_Addr, n)

/* 输入 */
#define PAin(n)   BIT_ADDR(GPIOA_IDR_Addr, n)
#define PBin(n)   BIT_ADDR(GPIOB_IDR_Addr, n)
#define PCin(n)   BIT_ADDR(GPIOC_IDR_Addr, n)
#define PDin(n)   BIT_ADDR(GPIOD_IDR_Addr, n)
#define PEin(n)   BIT_ADDR(GPIOE_IDR_Addr, n)
#define PFin(n)   BIT_ADDR(GPIOF_IDR_Addr, n)
#define PGin(n)   BIT_ADDR(GPIOG_IDR_Addr, n)
#define PHin(n)   BIT_ADDR(GPIOH_IDR_Addr, n)
#define PIin(n)   BIT_ADDR(GPIOI_IDR_Addr, n)

/*==============================================================================
 *  延时函数
 *  系统主频 168MHz（由 CubeMX 的 SystemClock_Config 配置）
 *============================================================================*/
void delay_init(void);          /* 初始化微秒延时（使能 DWT 周期计数器） */
void delay_ms(u16 nms);
void delay_us(u32 nus);

#endif
