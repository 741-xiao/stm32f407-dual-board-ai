# -*- coding: utf-8 -*-
"""gen_hanzi16.py —— 生成 16x16 汉字点阵（宋体），输出为 chinese16.c 的 C 数组

与 gen_hanzi.py（24x24 版）不同：本脚本生成 **16x16** 小号中文字模，用于
实验六"屏幕放不下 → 全面缩小字号"的新布局。

数据格式：每字 16 个 u16；u16 的 bit15 = 最左像素（0x8000 >> col）；
          下标 = 点阵第 0..15 行（上到下）。

用法：
    python tools/gen_hanzi16.py                 # 生成内置字表
    python tools/gen_hanzi16.py 温 度 湿 光 敏  # 自定义
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "C:/Windows/Fonts/simsun.ttc"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "firmware", "USER", "Chinese", "chinese16.c")

# 实验六 UI 用到的全部汉字（顺序即字库下标，务必与 chinese16.h 的宏一致）
CHARS = list("温度湿光敏甲醛本板状态正常错误消息数据采集开始停止按键报警关闭姓名学号更新")

SIZE = 16
THR = 128


def render(ch):
    img = Image.new("L", (16, 16), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=ImageFont.truetype(FONT, SIZE, index=0), fill=255)
    px = img.load()
    out = []
    for row in range(16):
        v = 0
        for col in range(16):
            if px[col, row] >= THR:
                v |= (0x8000 >> col)
        out.append(v)
    return out


def preview(vals):
    for v in vals:
        print("    " + "".join("##" if (v & (0x8000 >> c)) else ".." for c in range(16)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    chars = args if args else CHARS
    lines = []
    for i, ch in enumerate(chars):
        vals = render(ch)
        lines.append("  /* %2d %s */ {%s}," % (i, ch, ",".join("0x%04X" % v for v in vals)))

    # 预览（人眼校验字形）
    print("==== 预览（前 6 字）====")
    for ch in chars[:6]:
        print("  [%s]" % ch)
        preview(render(ch))

    body = "\n".join(lines)
    print("\n==== C 数组（%d 字）====" % len(chars))
    print(body)

    if "--write" in sys.argv:
        with open(OUT, "w", encoding="utf-8", newline="\n") as f:
            f.write(TEMPLATE % body)
        print("\n已写入 %s" % OUT)


TEMPLATE = """#include "lcd.h"
#include "chinese16.h"

extern u16 BRUSH_COLOR;
extern u16 BACK_COLOR;

/*==============================================================================
 *  chinese16.c  ---  16x16 中文字模（宋体，由 tools/gen_hanzi16.py 生成）
 *
 *  为什么另做一套：实验六屏幕要显示三块板 · 5 项数据，24x24 中文（实占 26x26）
 *  太占地方。改用 16x16 后，行距可压到 20，一屏能排 15+ 行。
 *
 *  数据格式：每字 16 个 u16，bit15 = 最左像素（0x8000 >> col），行自上而下。
 *============================================================================*/

static const u16 hanzi16[][16] =
{
%s
};

void LCD_ShowChinese16(u16 x, u16 y, u8 index)
{
    u8 row, col;
    u16 bits;
    for (row = 0U; row < 16U; row++)
    {
        bits = hanzi16[index][row];
        for (col = 0U; col < 16U; col++)
        {
            LCD_Color_DrawPoint((u16)(x + col), (u16)(y + row),
                                ((bits & ((u16)0x8000U >> col)) != 0U) ? BRUSH_COLOR : BACK_COLOR);
        }
    }
}

void LCD_ShowChinese16_color(u16 x, u16 y, u8 index, u16 fc, u16 bc)
{
    u8 row, col;
    u16 bits;
    for (row = 0U; row < 16U; row++)
    {
        bits = hanzi16[index][row];
        for (col = 0U; col < 16U; col++)
        {
            LCD_Color_DrawPoint((u16)(x + col), (u16)(y + row),
                                ((bits & ((u16)0x8000U >> col)) != 0U) ? fc : bc);
        }
    }
}
"""


if __name__ == "__main__":
    main()
