#ifndef __KEY_H
#define __KEY_H

#include "common.h"

/*==============================================================================
 *  按键 IO 端口定义
 *  初始化已由 CubeMX 的 MX_GPIO_Init() 完成：
 *      PF6 / PF7 / PF8 / PF9  ->  输入，上拉（按下为低电平）
 *  因此这里不再提供 KEY_Init()。
 *
 *  ★ 板上是「5 向按键」：四个方向 = KEY0..KEY3，中间键 = RESET（见原理图）。
 *    以下名称与**板上网络名一致**（不要再叫"按键1/2/3/4"，容易差一位）。
 *============================================================================*/
#define KEY0    PFin(9)     /* PF9  开始采集（仅板1） */
#define KEY1    PFin(8)     /* PF8  停止采集（仅板1） */
#define KEY2    PFin(7)     /* PF7  切换 数据页/状态页（两板通用） */
#define KEY3    PFin(6)     /* PF6  手动动作：板1 重发 CAN / 板2 恢复 I2C */

/* 按键返回值定义 */
#define KEY0_DATA   1
#define KEY1_DATA   2
#define KEY2_DATA   3
#define KEY3_DATA   4

/* 对外变量 */
extern u8  keydown_data;    /* 按下后返回的键值（长按/连发） */
extern u8  keyup_data;      /* 抬起时返回的键值 */
extern u16 key_time;        /* 本次按下的累计扫描次数 */
extern u8  key_tem;         /* 临时键值 */

/* 按键扫描函数
 *   mode = 0 : 不支持连发（长按只响应一次）
 *   mode = 1 : 支持连发
 *   必须周期调用（例如每 10ms 一次） */
void key_scan(u8 mode);

#endif
