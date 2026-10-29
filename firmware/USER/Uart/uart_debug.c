#include "uart_debug.h"
#include <stdarg.h>

extern UART_HandleTypeDef huart1;       /* 由 Core/Src/usart.c 提供 */

#define UART_TX_TIMEOUT     100U
#define UART_OUT_SIZE       192U

/*==============================================================================
 *  发送互斥的"锁钩子"（详见 uart_debug.h）
 *
 *  默认**空实现**：裸机 / 板2 是单线程，不需要锁。
 *  FreeRTOS 工程里 USER/Uart/uart_lock.c 会给同名函数一份**强定义**，
 *  链接时覆盖掉这里的弱定义 —— 所以**一份源码不用为"有没有 RTOS"分叉**。
 *============================================================================*/
__weak void UART_DebugLockInit(void)
{
}

__weak void UART_TxLock(void)
{
}

__weak void UART_TxUnlock(void)
{
}

void UART_DebugInit(void)
{
    UART_DebugLockInit();
}

/* 无符号整数转字符串（定宽、左补 0） */
static u16 UART_PutU32(char *out, u32 value, u32 base, u32 width, u8 upper)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *digits = (upper != 0U) ? upper_digits : lower_digits;
    char tmp[12];
    u32  n = 0U;
    u16  len = 0U;
    u32  i;

    /* 32 位十进制最多 10 位；这里封顶保证 tmp[12] 不会越界 */
    if (width > 10U)
    {
        width = 10U;
    }

    if (value == 0U)
    {
        tmp[n++] = '0';
    }
    while (value != 0U)
    {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    while (n < width)
    {
        tmp[n++] = '0';
    }
    for (i = n; i > 0U; i--)
    {
        out[len++] = tmp[i - 1U];
    }
    return len;
}

void UART_Puts(const char *s)
{
    u16 len = 0U;

    while ((s[len] != '\0') && (len < 250U))
    {
        len++;
    }
    if (len != 0U)
    {
        UART_TxLock();
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, len, UART_TX_TIMEOUT);
        UART_TxUnlock();
    }
}

void UART_PutsN(const u8 *buf, u16 len)
{
    if (len != 0U)
    {
        UART_TxLock();
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, UART_TX_TIMEOUT);
        UART_TxUnlock();
    }
}

void UART_Printf(const char *fmt, ...)
{
    char    out[UART_OUT_SIZE];
    u16     n = 0U;
    const char *p = fmt;
    va_list ap;

    va_start(ap, fmt);

    while ((*p != '\0') && (n < (u16)(UART_OUT_SIZE - 12U)))
    {
        if (*p != '%')
        {
            out[n++] = *p++;
            continue;
        }
        p++;                                        /* 跳过 '%' */
        if (*p == '%')
        {
            out[n++] = '%';
            p++;
            continue;
        }

        /* 解析可选的定宽，例如 %04X 里的 4
         * 注意：必须从 0 开始累加，否则 %04X 会算成 104、%08X 会算成 108，
         *       接着 UART_PutU32 里的补零循环就会把栈写穿（踩掉返回地址 → HardFault）。 */
        {
            u32 width = 0U;
            while ((*p >= '0') && (*p <= '9'))
            {
                width = (width * 10U) + (u32)(*p - '0');
                p++;
                if (width > 32U)        /* 防御：不合理的宽度直接封顶 */
                {
                    width = 32U;
                }
            }
            if (width == 0U)            /* 没写宽度时默认 1 位 */
            {
                width = 1U;
            }

            switch (*p)
            {
                case 'd':
                {
                    s32 v = va_arg(ap, s32);
                    if (v < 0)
                    {
                        out[n++] = '-';
                        v = -v;
                    }
                    n += UART_PutU32(&out[n], (u32)v, 10U, width, 0U);
                    break;
                }
                case 'u':
                    n += UART_PutU32(&out[n], va_arg(ap, u32), 10U, width, 0U);
                    break;
                case 'x':
                    n += UART_PutU32(&out[n], va_arg(ap, u32), 16U, width, 0U);
                    break;
                case 'X':
                    n += UART_PutU32(&out[n], va_arg(ap, u32), 16U, width, 1U);
                    break;
                case 'c':
                    out[n++] = (char)va_arg(ap, s32);
                    break;
                case 's':
                {
                    const char *sp = va_arg(ap, const char *);
                    while ((*sp != '\0') && (n < (u16)(UART_OUT_SIZE - 1U)))
                    {
                        out[n++] = *sp++;
                    }
                    break;
                }
                default:
                    out[n++] = *p;
                    break;
            }
            if (*p != '\0')
            {
                p++;
            }
        }
    }

    va_end(ap);

    if (n != 0U)
    {
        UART_TxLock();
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)out, n, UART_TX_TIMEOUT);
        UART_TxUnlock();
    }
}

/*==============================================================================
 *  故障处理专用输出：直接操作 USART1 寄存器，框架极小（无栈数组、无 HAL、无 va_arg）
 *  进 HardFault 后 MSP 可能已经不可靠，所以只能用这种最"裸"的方式。
 *  ★ 这几个**永远不上锁**（故障上下文里锁没意义、也不能阻塞）。
 *============================================================================*/
void UART_RawPutc(u8 c)
{
    while ((USART1->SR & USART_SR_TXE) == 0U)
    {
    }
    USART1->DR = (u32)c;
}

void UART_RawPuts(const char *s)
{
    while (*s != 0)
    {
        UART_RawPutc((u8)(*s));
        s++;
    }
}

void UART_RawHex32(u32 value)
{
    u8 i;
    u8 nibble;

    for (i = 8U; i > 0U; i--)
    {
        nibble = (u8)((value >> ((i - 1U) * 4U)) & 0x0FU);
        UART_RawPutc((u8)((nibble < 10U) ? (u8)('0' + nibble) : (u8)('A' + nibble - 10U)));
    }
}

/*==============================================================================
 *  交互式串口调试：接收（USART1 RX 轮询）
 *  回显用 UART_RawPutc（直接写 DR），不经过 HAL_UART_Transmit 的阻塞路径。
 *============================================================================*/
#define DBG_LINE_MAX    48U

static char dbg_line[DBG_LINE_MAX];     /* 正在拼的行 */
static u16  dbg_len   = 0U;             /* 已收字符数 */
static u8   dbg_ready = 0U;             /* 1 = 有拼好的整行待取 */
static char dbg_done[DBG_LINE_MAX];     /* 拼好的整行（等 GetLine 取走）*/

u8 UartDebug_GetLine(char *out, u16 max)
{
    u16 i;

    /* 1) 把 USART1 收到的字节一次取完（轮询 RXNE；读 DR 清标志）*/
    while ((USART1->SR & USART_SR_RXNE) != 0U)
    {
        u8 c = (u8)(USART1->DR & 0xFFU);

        if ((c == 13U) || (c == 10U))               /* CR / LF：一行结束 */
        {
            UART_RawPutc(13U);
            UART_RawPutc(10U);                      /* 回显换行 */
            if (dbg_len != 0U)                      /* 空行忽略 */
            {
                for (i = 0U; i < dbg_len; i++)
                {
                    dbg_done[i] = dbg_line[i];
                }
                dbg_done[dbg_len] = '\0';
                dbg_ready = 1U;
                dbg_len   = 0U;
            }
        }
        else if ((c == 8U) || (c == 127U))          /* 退格：删一个字符 */
        {
            if (dbg_len != 0U)
            {
                dbg_len--;
                UART_RawPutc(8U);                   /* 回显 "退格 空格 退格" */
                UART_RawPutc(32U);
                UART_RawPutc(8U);
            }
        }
        else if ((c >= 32U) && (c <= 126U))         /* 可见字符：入队并回显 */
        {
            UART_RawPutc(c);
            if (dbg_len < (DBG_LINE_MAX - 1U))      /* 满了就丢尾字符，不越界 */
            {
                dbg_line[dbg_len] = (char)c;
                dbg_len++;
            }
        }
        /* 其它控制字符忽略 */
    }

    /* 2) 有整行就拷出去（最多 max-1 个字符）*/
    if (dbg_ready != 0U)
    {
        dbg_ready = 0U;
        for (i = 0U; (i < (u16)(max - 1U)) && (dbg_done[i] != '\0'); i++)
        {
            out[i] = dbg_done[i];
        }
        out[i] = '\0';
        return 1U;
    }
    return 0U;
}
