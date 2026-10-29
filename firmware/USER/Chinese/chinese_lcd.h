#ifndef __CHINESE_LCD_H
#define __CHINESE_LCD_H

#include "common.h"

/* Built-in 24x24 glyph indexes: 温 度 湿 姓 名 学 号 摄 氏 百 分 比 郑 鑫 更 新 正 常 错 误. */
#define LCD_HANZI_ZHENG  16U
#define LCD_HANZI_CHANG  17U
#define LCD_HANZI_CUO    18U
#define LCD_HANZI_WU     19U
#define LCD_HANZI_AN     20U   /* 按 */
#define LCD_HANZI_JIAN   21U   /* 键 */
#define LCD_HANZI_KAI    22U   /* 开 */
#define LCD_HANZI_SHI    23U   /* 始 */
#define LCD_HANZI_CAI    24U   /* 采 */
#define LCD_HANZI_JI     25U   /* 集 */
#define LCD_HANZI_TING   26U   /* 停 */
#define LCD_HANZI_ZHI    27U   /* 止 */
#define LCD_HANZI_ZHUANG 28U   /* 状 */
#define LCD_HANZI_TAI    29U   /* 态 */
#define LCD_HANZI_BAO    30U   /* 报 */
#define LCD_HANZI_JING   31U   /* 警 */

/* ---- 姓名用字（下标 32 起，按需选用）---- */
#define LCD_HANZI_XIAO   32U   /* 肖 */
#define LCD_HANZI_SU     33U   /* 肃 */
#define LCD_HANZI_LIU    34U   /* 刘 */
#define LCD_HANZI_XING   35U   /* 星 */
#define LCD_HANZI_YU     36U   /* 宇 */
#define LCD_HANZI_LI     37U   /* 李 */
#define LCD_HANZI_ZHONG  38U   /* 钟 */
#define LCD_HANZI_HAI    39U   /* 海 */
#define LCD_HANZI_CHEN   40U   /* 陈 */
#define LCD_HANZI_HAO    41U   /* 浩 */
#define LCD_HANZI_XI     42U   /* 曦 */
#define LCD_HANZI_LIANG  43U   /* 亮 */

#define LCD_HANZI_ZHENG_G  12U   /* 郑（含义同上，给姓名表用）*/
#define LCD_HANZI_XIN_G    13U   /* 鑫 */

#define LCD_HANZI_GUANG  44U   /* 光 */
#define LCD_HANZI_MIN    45U   /* 敏 */

/* ---- 实验五新增（分区显示/文案；字模由 tools/gen_hanzi.py 生成）---- */
#define LCD_HANZI_BEN    46U   /* 本 */
#define LCD_HANZI_BAN    47U   /* 板 */
#define LCD_HANZI_DUI    48U   /* 对 */
#define LCD_HANZI_FANG   49U   /* 方 */
#define LCD_HANZI_GUAN   50U   /* 关 */
#define LCD_HANZI_BI     51U   /* 闭 */

/* ---- 实验六新增（SGP30 的"甲醛"标签）---- */
#define LCD_HANZI_JIA    52U   /* 甲 */
#define LCD_HANZI_QUAN   53U   /* 醛 */

void LCD_DisplayChinese(u16 x, u16 y, u8 index);

#endif
