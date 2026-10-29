# -*- coding: utf-8 -*-
"""gen_lcd_preview.py —— 把实验六的 LCD 实际显示效果渲染成 HTML

★ 关键：不是用系统字体"模拟"，而是**直接解析固件里的点阵**渲染：
    - 中文字模：firmware/USER/Chinese/chinese16.c （16x16，每字 16 个 u16，bit15=最左）
    - ASCII 字模：firmware/USER/LCD/cfont.h 的 char_1206 （95 字 x 12 字节，6 宽，LSB 在前）
    画法与 LCD_DisplayChar / LCD_ShowChinese16 完全一致 —— 所以看到的就是屏上真样子。

布局常量取自 app_node.c（改坐标时这里要同步）：
    Y_ROW / X_LABEL / X_COLON / X_VALUE / X_UNIT / X_NUM / X_TAG / ASC_W

用法：python tools/gen_lcd_preview.py       # 输出 docs/lcd_preview.html
"""
import io
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CN_SRC = os.path.join(ROOT, "firmware", "USER", "Chinese", "chinese16.c")
ASCII_SRC = os.path.join(ROOT, "firmware", "USER", "LCD", "cfont.h")
OUT = os.path.join(ROOT, "docs", "lcd_preview.html")

# ---- 布局（与 app_node.c 同步）----
Y_ROW = [6, 30, 54, 78, 102, 126, 150, 174, 198, 222, 246, 270]
X_LABEL, X_COLON, X_VALUE, X_UNIT = 6, 42, 52, 92
X_NUM, X_TAG, ASC_W, CN_W = 32, 40, 6, 16

# ---- 颜色（RGB565 -> RGB888）----
C = {
    "blue":   "#0000FF",   # 0x001F
    "green":  "#00FF00",   # 0x07E0
    "red":    "#FF0000",   # 0xF800
    "gray":   "#848684",   # 0x8430
    "orange": "#FFA500",   # 0xFD20
    "white":  "#FFFFFF",
}

# ---- 中文下标（与 chinese16.h 同步）----
WEN, DU, SHI, GUANG, MIN, JIA, QUAN, BEN, BAN = 0, 1, 2, 3, 4, 5, 6, 7, 8
ZHUANG, TAI, ZHENG, CHANG, CUO, WU, XIAO, XI = 9, 10, 11, 12, 13, 14, 15, 16
SHU, JU, CAI, JI, KAI, SHI2, TING, ZHI = 17, 18, 19, 20, 21, 22, 23, 24
AN, JIAN, BAO, JING, GUAN, BI = 25, 26, 27, 28, 29, 30


def load_cn():
    src = io.open(CN_SRC, encoding="utf-8").read()
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{4}\s*,?){16})\s*\}", src)
    out = []
    for r in rows:
        out.append([int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", r)])
    return out


def load_ascii():
    src = io.open(ASCII_SRC, encoding="utf-8").read()
    m = re.search(r"char_1206\[95\]\[12\]\s*=\s*\{(.*?)\n\};", src, re.S)
    body = m.group(1)
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?){12})\s*\}", body)
    out = []
    for r in rows:
        out.append([int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", r)])
    return out


# ---- 屏幕内容（照 app_node.c 的绘制顺序）----
def screen_b1_data():
    s = []
    s += [("cn", X_LABEL, Y_ROW[0], WEN), ("cn", X_LABEL + CN_W, Y_ROW[0], DU),
          ("as", X_COLON, Y_ROW[0], ":"), ("as", X_UNIT, Y_ROW[0], "C")]
    s += [("cn", X_LABEL, Y_ROW[1], SHI), ("cn", X_LABEL + CN_W, Y_ROW[1], DU),
          ("as", X_COLON, Y_ROW[1], ":"), ("as", X_UNIT, Y_ROW[1], "%RH")]
    s += [("cn", X_LABEL, Y_ROW[2], GUANG), ("cn", X_LABEL + CN_W, Y_ROW[2], MIN),
          ("as", X_COLON, Y_ROW[2], ":"), ("as", X_UNIT, Y_ROW[2], "%")]
    s += [("as", X_LABEL, Y_ROW[3], "CO2"), ("as", X_UNIT, Y_ROW[3], "ppm")]
    s += [("cn", X_LABEL, Y_ROW[4], JIA), ("cn", X_LABEL + CN_W, Y_ROW[4], QUAN),
          ("as", X_UNIT, Y_ROW[4], "ppb")]
    s += [("line", 122, "gray")]
    s += [("as", X_LABEL, Y_ROW[6], "NO"),
          ("cn", X_LABEL, Y_ROW[7], ZHUANG), ("cn", X_LABEL + CN_W, Y_ROW[7], TAI),
          ("cn", X_LABEL, Y_ROW[8], BAN), ("as", X_LABEL + CN_W, Y_ROW[8], "2"),
          ("cn", X_LABEL, Y_ROW[9], BAN), ("as", X_LABEL + CN_W, Y_ROW[9], "3")]
    s += [("line", 242, "gray")]
    s += [("as", X_LABEL, Y_ROW[11], "WiFi"),
          ("cn", 96, Y_ROW[11], XIAO), ("cn", 96 + CN_W, Y_ROW[11], XI)]
    # 动态值（示例）
    s += [("as", X_VALUE, Y_ROW[0], "25.3"), ("as", X_VALUE, Y_ROW[1], "60.2"),
          ("as", X_VALUE, Y_ROW[2], " 45"), ("as", X_VALUE, Y_ROW[3], "  512"),
          ("as", X_VALUE, Y_ROW[4], "   23"), ("as", X_NUM, Y_ROW[6], "0001")]
    s += [("cn", X_VALUE, Y_ROW[7], ZHENG, "green"), ("cn", X_VALUE + CN_W, Y_ROW[7], CHANG, "green")]
    s += [("cn", X_TAG, Y_ROW[8], ZHENG, "green"), ("cn", X_TAG + CN_W, Y_ROW[8], CHANG, "green")]
    s += [("cn", X_TAG, Y_ROW[9], CUO, "red"), ("cn", X_TAG + CN_W, Y_ROW[9], WU, "red")]
    s += [("cn", X_TAG, Y_ROW[11], ZHENG, "green"), ("cn", X_TAG + CN_W, Y_ROW[11], CHANG, "green")]
    for i, idx in enumerate([KAI, SHI2, CAI, JI]):
        s.append(("cn", 128 + i * CN_W, Y_ROW[11], idx))
    return "板1 · 数据页（默认）", s


def screen_status(board):
    """状态页：板1(board=1) 与 从机(board=2/3) 只差 r6/r7/r10。"""
    s = []
    s += [("cn", X_LABEL, Y_ROW[0], ZHUANG), ("cn", X_LABEL + CN_W, Y_ROW[0], TAI)]
    s += [("as", 6, Y_ROW[1], "WiFi"), ("as", 46, Y_ROW[1], "READY"),
          ("cn", 170, Y_ROW[1], ZHENG, "green"), ("cn", 186, Y_ROW[1], CHANG, "green")]
    s += [("as", 6, Y_ROW[2], "CAN TX"), ("as", 66, Y_ROW[2], "00012")]
    s += [("as", 6, Y_ROW[3], "CAN RX"), ("as", 66, Y_ROW[3], "00007")]
    s += [("as", 6, Y_ROW[4], "SHT30"),
          ("cn", 60, Y_ROW[4], ZHENG, "green"), ("cn", 76, Y_ROW[4], CHANG, "green"),
          ("as", 110, Y_ROW[4], "fail"), ("as", 140, Y_ROW[4], "000")]
    s += [("as", 6, Y_ROW[5], "SGP30"),
          ("cn", 60, Y_ROW[5], ZHENG, "green"), ("cn", 76, Y_ROW[5], CHANG, "green"),
          ("as", 110, Y_ROW[5], "warm"), ("as", 146, Y_ROW[5], "1")]
    if board == 1:
        s += [("as", 6, Y_ROW[6], "SLAVE2"),
              ("cn", 70, Y_ROW[6], ZHENG, "green"), ("cn", 86, Y_ROW[6], CHANG, "green")]
        s += [("as", 6, Y_ROW[7], "SLAVE3"),
              ("cn", 70, Y_ROW[7], CUO, "red"), ("cn", 86, Y_ROW[7], WU, "red")]
    else:
        s += [("as", 6, Y_ROW[6], "MASTER1"),
              ("cn", 70, Y_ROW[6], ZHENG, "green"), ("cn", 86, Y_ROW[6], CHANG, "green")]
        s += [("as", 6, Y_ROW[7], "PEER ON"), ("as", 70, Y_ROW[7], "1")]
    s += [("as", 6, Y_ROW[8], "NO"), ("as", 30, Y_ROW[8], "00045")]
    s += [("as", 6, Y_ROW[9], "RUN"), ("as", 30, Y_ROW[9], "   312"), ("as", 70, Y_ROW[9], "s")]
    s += [("as", 6, Y_ROW[10], "BOARD"), ("as", 42, Y_ROW[10], str(board))]
    s += [("as", 6, Y_ROW[11], "KEY2:page KEY3:act")]
    return ("板%d · 状态页（KEY2 切换）" % board), s


def screen_slave_data(title_color="green"):
    s = []
    s += [("cn", X_LABEL, Y_ROW[0], BEN, "blue"), ("cn", X_LABEL + CN_W, Y_ROW[0], BAN, "blue")]
    for i, (a, b, unit) in enumerate([(WEN, DU, "C"), (SHI, DU, "%RH"),
                                      (GUANG, MIN, "%")]):
        s += [("cn", X_LABEL, Y_ROW[1 + i], a), ("cn", X_LABEL + CN_W, Y_ROW[1 + i], b),
              ("as", X_COLON, Y_ROW[1 + i], ":"), ("as", X_UNIT, Y_ROW[1 + i], unit)]
    s += [("as", X_LABEL, Y_ROW[4], "CO2"), ("as", X_UNIT, Y_ROW[4], "ppm")]
    s += [("cn", X_LABEL, Y_ROW[5], JIA), ("cn", X_LABEL + CN_W, Y_ROW[5], QUAN),
          ("as", X_UNIT, Y_ROW[5], "ppb")]
    s += [("line", 146, "gray")]
    s += [("cn", X_LABEL, Y_ROW[6], BAN, title_color), ("as", X_LABEL + CN_W, Y_ROW[6], "1", title_color)]
    for i, (a, b, unit) in enumerate([(WEN, DU, "C"), (SHI, DU, "%RH"), (GUANG, MIN, "%")]):
        s += [("cn", X_LABEL, Y_ROW[7 + i], a), ("cn", X_LABEL + CN_W, Y_ROW[7 + i], b),
              ("as", X_COLON, Y_ROW[7 + i], ":"), ("as", X_UNIT, Y_ROW[7 + i], unit)]
    s += [("as", X_LABEL, Y_ROW[10], "CO2"), ("as", X_UNIT, Y_ROW[10], "ppm")]
    s += [("cn", X_LABEL, Y_ROW[11], JIA), ("cn", X_LABEL + CN_W, Y_ROW[11], QUAN),
          ("as", X_UNIT, Y_ROW[11], "ppb")]
    # 本板值
    for i, v in enumerate(["25.3", "60.2", " 45", "  512", "   23"]):
        s.append(("as", X_VALUE, Y_ROW[1 + i], v))
    # 板1 区值
    for i, v in enumerate(["26.1", "58.2", " 45", "  623", "   12"]):
        s.append(("as", X_VALUE, Y_ROW[7 + i], v))
    return "板2 / 板3 · 数据页", s


def main():
    cn = load_cn()
    asc = load_ascii()
    print("中文点阵 %d 个，ASCII 点阵 %d 个" % (len(cn), len(asc)))
    assert len(cn) == 37, "中文点阵数不对"
    assert len(asc) == 95, "ASCII 点阵数不对"

    screens = [
        screen_b1_data(),
        screen_status(1),
        screen_slave_data("green"),
        screen_status(2),
    ]

    import json
    data = json.dumps(screens, ensure_ascii=False)

    html = TEMPLATE.replace("__CN__", json.dumps(cn)).replace(
        "__ASC__", json.dumps(asc)).replace("__SCREENS__", data).replace(
        "__COLORS__", json.dumps(C))
    io.open(OUT, "w", encoding="utf-8", newline="\n").write(html)
    print("已写入 %s" % OUT)


TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>实验六 · LCD 显示效果（按固件点阵渲染）</title>
<style>
 body{font-family:system-ui,"Microsoft YaHei",sans-serif;margin:18px;background:#15161a;color:#e8e8e8}
 h1{font-size:17px;font-weight:500;margin:0 0 4px}
 p.note{font-size:13px;color:#9aa0a6;margin:0 0 16px}
 .row{display:flex;flex-wrap:wrap;gap:22px}
 .cell{text-align:center}
 .cell .cap{font-size:13px;color:#c8ccd2;margin-bottom:6px}
 canvas{width:288px;height:384px;image-rendering:pixelated;border:6px solid #33353b;border-radius:6px;background:#fff}
 .legend{font-size:12px;color:#7d838a;margin-top:6px}
</style></head><body>
<h1>实验六 · LCD 显示效果</h1>
<p class="note">按固件里的**实际点阵**逐像素渲染（中文 16×16 / ASCII 12 号），坐标与 app_node.c 一致；数值为示例值。</p>
<div class="row" id="row"></div>
<p class="legend">注：屏上文字为纯蓝 #0000FF；「正常」绿、「错误」红由固件里 App_ShowOK 上色；分隔线为灰色。</p>
<script>
const CN=__CN__, ASCII=__ASC__, SCREENS=__SCREENS__, COL=__COLORS__;
function mk(w,h){const c=document.createElement('canvas');c.width=w;c.height=h;return c;}
function draw(canvas, ops){
  const g=canvas.getContext('2d');
  g.fillStyle=COL.white; g.fillRect(0,0,240,320);
  const px=(x,y,col)=>{g.fillStyle=COL[col]||col;g.fillRect(x,y,1,1);};
  for(const op of ops){
    const kind=op[0];
    if(kind==='as'){
      const [_,x0,y0,text,col]=op; const c=col||'blue';
      const fm=ASCII;
      for(let i=0;i<text.length;i++){
        const idx=text.charCodeAt(i)-32; if(idx<0||idx>94) continue;
        const gl=fm[idx]; let x=x0+i*6,y=y0;
        for(let b=0;b<12;b++){ const by=gl[b];
          for(let a=0;a<8;a++){ if(by&(1<<a)) px(x,y,c); x++;
            if(x-(x0+i*6)===6){ x=x0+i*6; y++; break; } }
        }
      }
    } else if(kind==='cn'){
      const [_,x0,y0,idxi,col]=op; const c=col||'blue';
      const gl=CN[idxi];
      for(let row=0;row<16;row++){ const bits=gl[row];
        for(let col2=0;col2<16;col2++){ if(bits&(0x8000>>col2)) px(x0+col2,y0+row,c); } }
    } else if(kind==='line'){
      const [_,y,col]=op; for(let x=6;x<=233;x++) px(x,y,col);
    }
  }
}
const row=document.getElementById('row');
for(const [title,ops] of SCREENS){
  const cell=document.createElement('div'); cell.className='cell';
  const cap=document.createElement('div'); cap.className='cap'; cap.textContent=title;
  const cv=mk(240,320); draw(cv,ops);
  cell.appendChild(cap); cell.appendChild(cv); row.appendChild(cell);
}
</script></body></html>
"""

if __name__ == "__main__":
    main()
