#include "uart_debug.h"

#include "FreeRTOS.h"
#include "semphr.h"

/*==============================================================================
 *  uart_lock.c  ---  FreeRTOS 版的"串口发送锁"（**只有 RTOS 工程有这个文件**）
 *
 *  uart_debug.c 里那三个 UART_*Lock* 是 __weak 的空实现；
 *  这里给出**强定义**在链接时把它覆盖掉 —— 多个任务都会打日志，发送必须串行。
 *
 *  为什么单独一个文件：uart_debug.c 是三块工程**共享的一份源码**，
 *  里面不能出现 FreeRTOS 头（裸机工程没有那套 include 路径）。
 *  用弱符号把"要不要上锁"交给各工程自己决定，共享文件就不用分叉。
 *============================================================================*/

static SemaphoreHandle_t uart_tx_mtx;

void UART_DebugLockInit(void)
{
    if (uart_tx_mtx == NULL)
    {
        uart_tx_mtx = xSemaphoreCreateMutex();
    }
}

void UART_TxLock(void)
{
    if (uart_tx_mtx != NULL)
    {
        (void)xSemaphoreTake(uart_tx_mtx, portMAX_DELAY);
    }
}

void UART_TxUnlock(void)
{
    if (uart_tx_mtx != NULL)
    {
        (void)xSemaphoreGive(uart_tx_mtx);
    }
}
