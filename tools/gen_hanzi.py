# -*- coding: utf-8 -*-
"""gen_hanzi.py —— 生成 chinese_lcd.c 的 24x24 汉字点阵（宋体），并保证与旧字同风格

方法（两步）：
  1) 校准：从 chinese_lcd.c 解析出**已有字模**（当作标准答案），用 Pillow 渲染
     同样的字，在 字号/偏移/阈值 的候选空间里搜索——取与旧字逐位（XOR）差异
     最小的参数组。这样新字与旧字出自"同一配方"。
  2) 生成：用最优参数渲染新字（默认 本 板 对 方 关 闭），打印可直接粘贴进
     chinese_lcd.c 的 C 数组行；同时打印 ASCII 预览供肉眼检查。

数据格式（与 chinese_lcd.c 一致）：
  每字 24 个 u32；u32 的 bit23 = 最左像素（显示代码 0x800000 >> col）；
  行 = 点阵的第 0..23 行（上到下）。

用法（在"实验5"目录下）：
    python tools/gen_hanzi.py                 # 生成默认 6 字
    python tools/gen_hanzi.py 本 板 对 方 新  # 自定义要生成的字
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "firmware", "USER", "Chinese", "chinese_lcd.c")
FONT = "C:/Windows/Fonts/simsun.ttc"

# 校准参考：(字符, 在字库里的下标)。选取风格一致、结构清晰的字。
REFS = [("按", 20), ("开", 22), ("光", 44), ("敏", 45)]

DEFAULT_CHARS = list("本板对方关闭")

_fonts = {}


def get_font(size):
    if size not in _fonts:
        _fonts[size] = ImageFont.truetype(FONT, size, index=0)
    return _fonts[size]


def load_glyphs():
    src = open(SRC, "rb").read().decode("utf-8", "ignore")
    rows = re.findall(r"\{((?:\s*0x[0-9A-Fa-f]{6}\s*,?){24})\}", src)
    glyphs = []
    for r in rows:
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", r)]
        glyphs.append(vals)
    return glyphs


def render(ch, size, ox, oy, thr):
    img = Image.new("L", (24, 24), 0)
    ImageDraw.Draw(img).text((ox, oy), ch, font=get_font(size), fill=255)
    px = img.load()
    out = []
    for row in range(24):
        v = 0
        for col in range(24):
            if px[col, row] >= thr:
                v |= (0x800000 >> col)
        out.append(v)
    return out


def diff(a, b):
    return sum(bin(x ^ y).count("1") for x, y in zip(a, b))


def calibrate(glyphs):
    best = None
    for size in range(21, 29):
        for ox in range(-4, 8):
            for oy in range(-6, 8):
                for thr in (100, 128, 156):
                    err = 0
                    for ch, idx in REFS:
                        err += diff(render(ch, size, ox, oy, thr), glyphs[idx])
                    if best is None or err < best[0]:
                        best = (err, size, ox, oy, thr)
    return best


def preview(vals):
    for v in vals:
        line = "".join("##" if (v & (0x800000 >> c)) else ".." for c in range(24))
        print("    " + line)


def main():
    glyphs = load_glyphs()
    print("已解析 %d 个旧字模" % len(glyphs))

    err, size, ox, oy, thr = calibrate(glyphs)
    print("校准结果: 字号=%d 偏移=(%d,%d) 阈值=%d  总位差=%d"
          % (size, ox, oy, thr, err))
    for ch, idx in REFS:
        e = diff(render(ch, size, ox, oy, thr), glyphs[idx])
        print("  参考字 '%s'(下标%d) 位差=%d" % (ch, idx, e))

    chars = sys.argv[1:] if len(sys.argv) > 1 else DEFAULT_CHARS
    print("\n==== 生成 %d 个新字 ====" % len(chars))
    for ch in chars:
        vals = render(ch, size, ox, oy, thr)
        print("  /* %s */ {%s}," % (ch, ",".join("0x%06X" % v for v in vals)))
        preview(vals)
        print("")


if __name__ == "__main__":
    main()
