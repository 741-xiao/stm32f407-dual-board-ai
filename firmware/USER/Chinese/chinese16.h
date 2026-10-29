#ifndef __CHINESE16_H
#define __CHINESE16_H

#include "common.h"

/*==============================================================================
 *  chinese16.h  ---  16x16 中文字模（小号字，实验六新布局用）
 *
 *  字模顺序与 tools/gen_hanzi16.py 的 CHARS 完全一致；改字表要两边同步。
 *  16x16 每字占 16x16 像素（比 24x24 的 26x26 小一半多），配合行距 20 使用。
 *============================================================================*/

#define H16_WEN     0U    /* 温 */
#define H16_DU      1U    /* 度 */
#define H16_SHI     2U    /* 湿 */
#define H16_GUANG   3U    /* 光 */
#define H16_MIN     4U    /* 敏 */
#define H16_JIA     5U    /* 甲 */
#define H16_QUAN    6U    /* 醛 */
#define H16_BEN     7U    /* 本 */
#define H16_BAN     8U    /* 板 */
#define H16_ZHUANG  9U    /* 状 */
#define H16_TAI    10U    /* 态 */
#define H16_ZHENG  11U    /* 正 */
#define H16_CHANG  12U    /* 常 */
#define H16_CUO    13U    /* 错 */
#define H16_WU     14U    /* 误 */
#define H16_XIAO   15U    /* 消 */
#define H16_XI     16U    /* 息 */
#define H16_SHU    17U    /* 数 */
#define H16_JU     18U    /* 据 */
#define H16_CAI    19U    /* 采 */
#define H16_JI     20U    /* 集 */
#define H16_KAI    21U    /* 开 */
#define H16_SHI2   22U    /* 始 */
#define H16_TING   23U    /* 停 */
#define H16_ZHI    24U    /* 止 */
#define H16_AN     25U    /* 按 */
#define H16_JIAN   26U    /* 键 */
#define H16_BAO    27U    /* 报 */
#define H16_JING   28U    /* 警 */
#define H16_GUAN   29U    /* 关 */
#define H16_BI     30U    /* 闭 */
#define H16_XING   31U    /* 姓 */
#define H16_MING   32U    /* 名 */
#define H16_XUE    33U    /* 学 */
#define H16_HAO    34U    /* 号 */
#define H16_GENG   35U    /* 更 */
#define H16_XIN    36U    /* 新 */

void LCD_ShowChinese16(u16 x, u16 y, u8 index);
void LCD_ShowChinese16_color(u16 x, u16 y, u8 index, u16 fc, u16 bc);

#endif
