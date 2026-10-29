#include "key.h"

/*==============================================================================
 *  按键扫描（状态机，非阻塞，约 10ms 调用一次）
 *
 *  ★ 设计目标：**一次物理按下 = 一次动作**，而且"轻点一下"也能触发。
 *
 *  工作方式：
 *    1. 第一次扫到某个键按下 -> 立即输出 keydown_data（短按即响应，不用按住）；
 *       同一键继续按住 -> mode=0 不再输出；mode=1 每拍输出（连发）。
 *    2. 松开后必须**连续 KEY_RELEASE_DEBOUNCE 拍**都读不到键，才把 key_armed
 *       重新置 1（"重新武装"）。
 *
 *  ★★ 为什么必须有第 2 条（老问题的根因）：
 *      轻触按键在按下/松开瞬间会机械抖动。若"只要有键就触发"，一次按下可能被
 *      判成两次。KEY0/KEY1 的动作是**幂等**的（重复"开始"/重复"停止"看不出问题），
 *      但 KEY2 的**切页是取反**的 —— 抖两次 = 切过去又切回来，屏幕上等于"没反应"。
 *      加了"松开去抖（重新武装）"后，抖动不会二次触发，四个键行为一致。
 *
 *  对外接口保持不变：keydown_data / keyup_data / key_tem / key_time
 *============================================================================*/

u8  keydown_data = 0x00;    /* 按下时返回的键值（短按即出；一次按住默认只出一次） */
u8  keyup_data   = 0x00;    /* 抬起时返回的键值（只在抬起那一拍有效） */
u16 key_time     = 0x00;    /* 本次按下的累计扫描次数（诊断用） */
u8  key_tem      = 0x00;    /* 本次识别到的键值 */

/* 松开去抖：连续多少次"无键按下"才允许下一次触发（2 拍 ≈ 20ms） */
#define KEY_RELEASE_DEBOUNCE    2U

static u8 key_armed  = 1U;  /* 1 = 空闲，可接受新按下；0 = 本次已触发，等松开重新武装 */
static u8 key_rel_cnt = 0U; /* 连续"无键按下"的扫描次数 */

/****************************************************************************
* 名    称: void key_scan(u8 mode)
* 功    能: 按键扫描函数
* 入口参数: mode：0 = 不支持连发；1 = 支持连发
* 说    明: 按键优先级 KEY0 > KEY1 > KEY2 > KEY3
****************************************************************************/
void key_scan(u8 mode)
{
    keyup_data = 0U;                            /* 抬起事件只在本次有效 */

    if (KEY0 == 0 || KEY1 == 0 || KEY2 == 0 || KEY3 == 0)   /* 有键按下 */
    {
        key_rel_cnt = 0U;                       /* 有键按下：松开计数清零 */

        if (KEY0 == 0)          key_tem = 1U;
        else if (KEY1 == 0)     key_tem = 2U;
        else if (KEY2 == 0)     key_tem = 3U;
        else                    key_tem = 4U;

        if (key_armed != 0U)                    /* ★ 新按下：本拍立即输出（短按即响应）*/
        {
            key_armed    = 0U;                  /* 未松开前不再触发 */
            key_time     = 1U;
            keydown_data = key_tem;
        }
        else if (mode != 0U)                    /* 连发模式：每拍都输出 */
        {
            key_time++;
            keydown_data = key_tem;
        }
        else                                    /* 非连发：按住不放不再输出 */
        {
            key_time++;
            keydown_data = 0U;
        }
    }
    else                                        /* 无键按下（已抬起） */
    {
        keydown_data = 0U;

        if (key_armed == 0U)                    /* 本次已触发过，等可靠松开后再武装 */
        {
            if (key_rel_cnt < 255U) { key_rel_cnt++; }
            if (key_rel_cnt >= KEY_RELEASE_DEBOUNCE)
            {
                keyup_data  = key_tem;          /* 产生一次"抬起"事件 */
                key_armed   = 1U;               /* ★ 重新武装，下一次按下可再触发 */
                key_rel_cnt = 0U;
            }
        }
        key_time = 0U;
    }
}
