# -*- coding: utf-8 -*-
"""make_project.py —— 让 Keil 工程长期可维护：**一个工程，两个 Target**

跑一次就保证下面几件事（幂等）：
  1) USER 分组把我们的 .c 都挂上（CubeMX 重新生成会把工程文件整个重写）；
  2) 有 board1 / board2 两个 Target，两者**参与编译的文件列表完全相同**，
     只有两处不同：C 的 Define（BOARD_SEL=1 / =2）和输出目录名；
  3) C 的 IncludePath 补上 USER 的各目录。

★ 实验七：需求只要求**两块** STM32F407（板1 主控 + 板2 从机），
  所以 TARGETS 从实验六的三块收敛为两块。
  本脚本会**先删掉 uvprojx 里所有 <Target> 再按 TARGETS 重建**，
  所以直接把 board3 从 TARGETS 里去掉就等于删掉那个 Target
  （不必手工改 XML —— 那套 XML 又脆又难维护）。

为什么两个 Target 文件列表一样：
    角色的差异**全部由 BOARD_SEL 宏分叉**——USER/App/app.c 和 USER/WIFI/esp01s.c
    里各有一个"选择器"，按宏 include 对应的实现。
    所以**不需要**在 Keil 里给每个 Target 单独排除文件。

用法：
    python tools/make_project.py firmware                      # 参数是工程目录
    python tools/make_project.py firmware firmware.uvprojx     # 可选：指定工程文件名

★ CubeMX 重新生成工程后必做：跑一次本脚本
  （否则第一个 Target 会被 CubeMX 改名成 firmware，BOARD_SEL 也会丢）。
"""
import os
import re
import shutil
import sys

CR = chr(13)
LF = chr(10)
EOL = CR + LF

USER_FILES = [
    ("common.c",      "../USER/Common/common.c"),
    ("key.c",         "../USER/KEY/key.c"),
    ("lcd.c",         "../USER/LCD/lcd.c"),
    ("chinese_lcd.c", "../USER/Chinese/chinese_lcd.c"),
    ("chinese16.c",   "../USER/Chinese/chinese16.c"),
    ("sht30.c",       "../USER/SHT30/sht30.c"),
    ("sgp30.c",       "../USER/SGP30/sgp30.c"),
    ("can_link.c",    "../USER/CAN/can_link.c"),
    ("light.c",       "../USER/Light/light.c"),
    ("led_effect.c",  "../USER/LED/led_effect.c"),
    ("app.c",         "../USER/App/app.c"),
    ("uart_debug.c",  "../USER/Uart/uart_debug.c"),
    ("uart_lock.c",   "../USER/Uart/uart_lock.c"),
    ("esp01s.c",      "../USER/WIFI/esp01s.c"),
    ("ai_model.c",    "../USER/AI/ai_model.c"),      # 题2：神经网络推理薄封装
    # ---- 题2：X-CUBE-AI 生成的网络代码（由 stedgeai generate 产出，别手改）----
    ("network.c",             "../USER/AI/network.c"),
    ("network_data.c",        "../USER/AI/network_data.c"),
    ("network_data_params.c", "../USER/AI/network_data_params.c"),
    # ---- 题2：AI 运行时库（Keil 版，AC5 编译的；只链进去用到的部分）----
    #      FileType=4 表示"库文件"（1 是 C 源文件）
    ("NetworkRuntime1020_CM4_Keil.lib",
     "../USER/AI/runtime/Lib/NetworkRuntime1020_CM4_Keil.lib", 4),
]

USER_INC = ["../USER/Common", "../USER/LED", "../USER/BEEP", "../USER/KEY",
            "../USER/LCD", "../USER/Chinese", "../USER/SHT30", "../USER/SGP30",
            "../USER/CAN", "../USER/App",
            "../USER/Uart", "../USER/WIFI", "../USER/Light",
            "../USER/AI",                            # 题2：ai_norm.h / ai_model.h 等
            "../USER/AI/runtime/Inc"]                # 题2：X-CUBE-AI 运行时的头文件

TARGETS = [("board1", 1), ("board2", 2)]


def build_group(mdk):
    out = ["        <Group>",
           "          <GroupName>USER</GroupName>",
           "          <Files>"]
    used, skipped = 0, []
    for item in USER_FILES:
        name, path = item[0], item[1]
        # 第三项可选：Keil 的 FileType —— 1 = C 源文件（默认），4 = 库文件(.lib)
        ftype = item[2] if len(item) > 2 else 1
        if not os.path.isfile(os.path.join(mdk, path.replace("/", os.sep))):
            skipped.append(name)
            continue
        used += 1
        out += ["            <File>",
                "              <FileName>%s</FileName>" % name,
                "              <FileType>%d</FileType>" % ftype,
                "              <FilePath>%s</FilePath>" % path,
                "            </File>"]
    out += ["          </Files>", "        </Group>"]
    return "\n".join(out), used, skipped


def find_uvprojx(root):
    md = os.path.join(root, "MDK-ARM")
    if not os.path.isdir(md):
        return None
    cands = sorted(f for f in os.listdir(md) if f.lower().endswith(".uvprojx"))
    if not cands:
        return None
    want = os.path.basename(os.path.normpath(root)) + ".uvprojx"
    if want in cands:
        return os.path.join(md, want)
    if len(cands) == 1:
        return os.path.join(md, cands[0])
    print("MDK-ARM 下有多个 uvprojx，请用第二个参数指定：%s" % ", ".join(cands))
    return None


def patch_target(t, name, sel, mdk):
    t = re.sub(r"<TargetName>[^<]*</TargetName>",
               lambda m: "<TargetName>%s</TargetName>" % name, t, count=1)
    t = re.sub(r"<OutputName>[^<]*</OutputName>",
               lambda m: "<OutputName>%s</OutputName>" % name, t, count=1)
    t = re.sub(r"<OutputDirectory>[^<]*</OutputDirectory>",
               lambda m: "<OutputDirectory>%s</OutputDirectory>" % (name + chr(92)),
               t, count=1)

    def defrep(m):
        parts = [p for p in m.group(1).split(",") if p and not p.startswith("BOARD_SEL")]
        parts.append("BOARD_SEL=%d" % sel)
        return "<Define>%s</Define>" % ",".join(parts)

    t = re.sub(r"<Define>([^<]*)</Define>", defrep, t, count=1)

    t = re.sub(r"[ \t]*<Group>\s*<GroupName>USER</GroupName>.*?</Group>\s*\n",
               "", t, flags=re.S)
    grp, nfiles, skipped = build_group(mdk)
    if "</Groups>" not in t:
        raise RuntimeError("没找到 </Groups>，uvprojx 结构可能变了")
    t = t.replace("</Groups>", grp + "\n      </Groups>", 1)

    def increp(m):
        cur = m.group(1)
        parts = [p for p in cur.split(";") if p]
        for d in USER_INC:
            if d not in parts and os.path.isdir(os.path.join(mdk, d.replace("/", os.sep))):
                parts.append(d)
        return "<IncludePath>%s</IncludePath>" % ";".join(parts)

    # ★ 必须替换**所有** <IncludePath>：一个 Target 里通常有 3 个
    #   —— TargetCommonOption 里一个、<Cads>（C 编译器，**真正生效**）里一个、
    #   <Aads>（汇编器）里一个。只改第一个（曾用 count=1）会漏掉 C 编译器那个，
    #   表现为"新加的 ../USER/XXX 目录没生效、找不到头文件"。
    t = re.sub(r"<IncludePath>([^<]*)</IncludePath>", increp, t)
    return t, nfiles, skipped


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    root = sys.argv[1]
    proj = (os.path.join(root, "MDK-ARM", sys.argv[2]) if len(sys.argv) >= 3
            else find_uvprojx(root))
    if not proj or not os.path.isfile(proj):
        print("找不到工程文件（%s/MDK-ARM）" % root)
        return 1
    mdk = os.path.join(root, "MDK-ARM")

    raw = open(proj, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    is_crlf = (EOL in raw.decode("latin-1"))
    txt = raw.decode("utf-8-sig").replace(EOL, LF)

    shutil.copyfile(proj, proj + ".pre_make.bak")

    blocks = list(re.finditer(r"[ \t]*<Target>.*?</Target>\s*\n", txt, re.S))
    if not blocks:
        print("!! 没找到 <Target>，uvprojx 结构可能变了")
        return 1
    tmpl = blocks[0].group(0)

    for m in reversed(blocks):
        txt = txt[:m.start()] + txt[m.end():]

    new, info = "", []
    for name, sel in TARGETS:
        b, nf, sk = patch_target(tmpl, name, sel, mdk)
        new += b
        info.append((name, sel, nf, sk))
    if "<Targets>" not in txt:
        print("!! 没找到 <Targets>")
        return 1
    txt = txt.replace("<Targets>" + LF, "<Targets>" + LF + new, 1)

    if is_crlf:
        txt = txt.replace(LF, EOL)
    open(proj, "w", encoding="utf-8-sig" if bom else "utf-8", newline="").write(txt)

    print("已改 %s" % proj)
    for name, sel, nf, sk in info:
        extra = ("  跳过(没有): %s" % ", ".join(sk)) if sk else ""
        print("  Target %-7s BOARD_SEL=%d  挂入 %d 个源文件%s" % (name, sel, nf, extra))
    print("  （原文件备份为 %s.pre_make.bak）" % os.path.basename(proj))
    return 0


if __name__ == "__main__":
    sys.exit(main())
