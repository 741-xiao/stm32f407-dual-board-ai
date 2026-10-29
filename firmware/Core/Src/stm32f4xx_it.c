/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "uart_debug.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/*==============================================================================
 *  ★ 故障现场上报（诊断用，交付前可删）
 *
 *  作用：CPU 进了任何 Fault 异常，**先把它自己"喊"出来**，而不是静默 while(1)。
 *        白屏 + 串口没输出 = 以前最常见的"无声死亡"，加了这个之后至少能拿到：
 *          CFSR  = 可配置故障状态（哪类故障、精确/不精确）
 *          HFSR  = 硬故障状态（FORCED 位说明是别的故障升级来的）
 *          MMFAR = 触发 MemManage 的地址
 *          BFAR  = 触发 BusFault 的地址  ← 写飞了哪个地址，通常一眼看出
 *          MSP/PSP = 出错时的栈指针（MSP 多半是启动阶段 / 中断里，PSP 是任务里）
 *          PC@MSP/PC@PSP = 两个栈上栈帧里的返回地址候选（配合 .map 反查函数）
 *
 *  为什么用 UART_Raw*：故障上下文里 HAL / 互斥锁 / 栈都可能不可靠，
 *  只有"直接写 USART1 数据寄存器"这条路最稳（无栈数组、无 va_arg、无锁）。
 *  （UART_Raw* 的声明已由文件顶部的 #include "uart_debug.h" 引入。）
 *============================================================================*/
static void Fault_Dump(const char *tag)
{
    u32 msp    = __get_MSP();
    u32 psp    = __get_PSP();
    /* 异常栈帧：{R0,R1,R2,R3,R12,LR,PC,xPSR}，PC 在 +0x18。
       注意：本函数自身已经压过栈，MSP 侧的 PC 只能算"候选"，仅作参考；
       真正的定位线索是 CFSR / BFAR / MMFAR。 */
    u32 pc_msp = *(volatile u32 *)(msp + 0x18U);
    u32 pc_psp = *(volatile u32 *)(psp + 0x18U);

    UART_RawPuts("\r\n!!!!!! [FAULT] ");
    UART_RawPuts(tag);
    UART_RawPuts(" !!!!!!\r\n");

    UART_RawPuts("CFSR  = "); UART_RawHex32(SCB->CFSR);
    UART_RawPuts("  HFSR = "); UART_RawHex32(SCB->HFSR);
    UART_RawPuts("\r\n");
    UART_RawPuts("MMFAR = "); UART_RawHex32(SCB->MMFAR);
    UART_RawPuts("  BFAR = "); UART_RawHex32(SCB->BFAR);
    UART_RawPuts("\r\n");
    UART_RawPuts("MSP   = "); UART_RawHex32(msp);
    UART_RawPuts("  PSP  = "); UART_RawHex32(psp);
    UART_RawPuts("\r\n");
    UART_RawPuts("PC@MSP= "); UART_RawHex32(pc_msp);
    UART_RawPuts("  PC@PSP="); UART_RawHex32(pc_psp);
    UART_RawPuts("\r\n");
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern UART_HandleTypeDef huart4;
extern UART_HandleTypeDef huart1;
extern TIM_HandleTypeDef htim6;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* 裸串口上报现场（不依赖 HAL/锁），避免二次故障把信息吞掉 */
  Fault_Dump("HARD FAULT");
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  Fault_Dump("MEM MANAGE FAULT");
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  Fault_Dump("BUS FAULT");
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  Fault_Dump("USAGE FAULT");
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles USART1 global interrupt.
  */
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */

  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
  /* USER CODE BEGIN USART1_IRQn 1 */

  /* USER CODE END USART1_IRQn 1 */
}

/**
  * @brief This function handles UART4 global interrupt.
  */
void UART4_IRQHandler(void)
{
  /* USER CODE BEGIN UART4_IRQn 0 */

  /* USER CODE END UART4_IRQn 0 */
  HAL_UART_IRQHandler(&huart4);
  /* USER CODE BEGIN UART4_IRQn 1 */

  /* USER CODE END UART4_IRQn 1 */
}

/**
  * @brief This function handles TIM6 global interrupt, DAC1 and DAC2 underrun error interrupts.
  */
void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */

  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */

  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
