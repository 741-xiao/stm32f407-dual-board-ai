#ifndef __UART_DEBUG_H
#define __UART_DEBUG_H

#include "common.h"

/*==============================================================================
 *  串口调试输出（USART1，PA9=TX / PA10=RX，115200-8-N-1）
 *  USART1 外设已由 CubeMX 的 MX_USART1_UART_Init() 初始化，这里只做发送。
 *  支持 %d %u %x %X %04X %c %s %% ，不支持浮点（定点数请拆成整数部分+小数部分）。
 *
 *  ★ 本模块三块工程共用（唯一来源：shared/USER/Uart/，用 tools/sync_shared.py 同步）。
 *    多任务串行发送靠下面那组"锁钩子"：**默认是空实现**（裸机/板2 不需要锁）；
 *    FreeRTOS 版在本工程内另给一份强定义（USER/Uart/uart_lock.c）把它覆盖掉。
 *============================================================================*/
void UART_Printf(const char *fmt, ...);
void UART_Puts(const char *s);
void UART_PutsN(const u8 *buf, u16 len);   /* 按长度发（中文字节数组用）*/

/* ---- 故障处理专用：不依赖 HAL、不用栈缓冲、直接写 USART1 寄存器 ----
 * 进 HardFault 后栈可能已经不可靠，所以这几个函数只用到寄存器。
 * ★ 这几个**永远不上锁**，保持裸寄存器写。 */
void UART_RawPutc(u8 c);
void UART_RawPuts(const char *s);
void UART_RawHex32(u32 value);

/*==============================================================================
 *  交互式串口调试：接收（USART1 RX，PA10）
 *
 *  ---- 为什么用轮询而不是中断 ----
 *  ESP(UART4) 已经占用了 HAL 的全局回调 HAL_UART_RxCpltCallback，
 *  再给 USART1 开中断就要么去改已验证的 esp01s.c（拆成按 Instance 分发），
 *  要么另起一个转发层；而调试台是人手敲、速率极低，主循环里轮询 RXNE
 *  完全够用，还不丢字节、不必动中断向量/it.c/esp01s.c。所以这里用轮询。
 *
 *  ---- 分工 ----
 *  本模块只做"传输层"：把收到的字节回显、拼成一行；命令的解析与执行
 *  放在 app.c（那里才拿得到采集状态、传感器、WiFi 状态）。
 *============================================================================*/

/* 主循环每圈调一次：收字节（含回显）并拼行。
 * 拼好一整行（回车/换行结束）时，把它拷进 out（不含 CR/LF）并返回 1；否则返回 0。
 * 空行忽略。out 建议给 48 字节以上。 */
u8 UartDebug_GetLine(char *out, u16 max);

/* ---- 发送互斥的"锁钩子"（弱符号；见文件头说明）----
 * 默认实现在 uart_debug.c 里（空函数），FreeRTOS 工程用 uart_lock.c 覆盖。
 * 调用方不用管：UART_Puts / UART_PutsN / UART_Printf 内部已加/解锁。 */
void UART_DebugLockInit(void);
void UART_TxLock(void);
void UART_TxUnlock(void);

/* 建发送锁；App_Init 里第一个调。（只对 RTOS 有意义，其它版本是空函数） */
void UART_DebugInit(void);

#endif
