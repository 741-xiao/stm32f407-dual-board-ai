#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_style.py —— 按《华为 C 语言编程规范》的**可自动化子集**扫固件源码

只查「机器能判定、且几乎不会误报」的几条，宁缺勿滥：

    [1-4]  一行只写一条语句
    [1-5]  if / for / while 的执行语句必须加 {}
    [1-6]  缩进只用空格，禁用 TAB
    [1-8]  大括号各独占一行（与语句左对齐）
    [1-9]  一行不宜过长（默认 120 字符；老版规范是 80）

不查的（需要人判断，脚本会误报）：
    命名是否合理、注释是否完整、函数是否过长、圈复杂度、宏是否有副作用……

用法：
    python tools\\check_style.py                    # 汇总 + 前 10 条样例
    python tools\\check_style.py --all              # 列出全部
    python tools\\check_style.py --max-len 80       # 按老版规范的 80 列
    python tools\\check_style.py --md docs\\代码规范自查.md   # 顺便存成 markdown
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "firmware", "USER")

# 这些目录是 ST 的库 / FreeRTOS，不是我们写的，不扫
SKIP_DIRS = {"Drivers", "Middlewares", "CMSIS", "FreeRTOS"}

# [1-8] 单行写完 if/for/while + 大括号 + 语句，例：if (d < 20U) { d = 20U; }
RE_INLINE_BLOCK = re.compile(r"^\s*(?:if|for|while|else\s+if)\b[^{;]*\{[^{}]*\}[^;{}]*;\s*$")
# [1-5] if/for/while 后面直接跟语句、没有大括号
RE_NO_BRACE = re.compile(r"^\s*(?:if|for|while)\s*\([^;]*\)\s*[A-Za-z_][^;{}]*;\s*$")
# [1-4] 一行里出现两条以上语句（分号后面还有内容，且不是注释）
RE_TWO_STMT = re.compile(r";\s*[A-Za-z_][A-Za-z0-9_]*\s*[=(]")


def read_text(path):
    """源码可能是 UTF-8 也可能是 GBK（Keil 默认 ANSI），都试一下。"""
    for enc in ("utf-8", "gbk"):
        try:
            with open(path, "r", encoding=enc) as f:
                return f.read()
        except UnicodeDecodeError:
            continue
        except OSError:
            return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def walk_sources():
    for dirpath, dirnames, filenames in os.walk(SRC_DIR):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn.lower().endswith((".c", ".h")):
                yield os.path.join(dirpath, fn)


def main():
    ap = argparse.ArgumentParser(description="按华为 C 语言编程规范自查（可自动化部分）")
    ap.add_argument("--max-len", type=int, default=120, help="行长上限（默认 120）")
    ap.add_argument("--all", action="store_true", help="列出全部问题，不只前 10 条")
    ap.add_argument("--md", default="", help="把结果存成 markdown（给文档用）")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    rules = {
        "1-4": ("一行只写一条语句", []),
        "1-5": ("if/for/while 缺大括号", []),
        "1-6": ("TAB 缩进", []),
        "1-8": ("大括号未独占一行", []),
        "1-9": ("行长超过 %d" % args.max_len, []),
    }

    n_files = n_lines = 0
    for path in sorted(walk_sources()):
        txt = read_text(path)
        if txt is None:
            continue
        n_files += 1
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        for i, line in enumerate(txt.splitlines(), 1):
            n_lines += 1
            s = line.rstrip()
            if not s.strip():
                continue
            # 跳过纯注释 / 预处理指令，减少误报
            stripped = s.lstrip()
            is_comment = stripped.startswith(("/*", "*", "//"))
            is_directive = stripped.startswith("#")

            if "\t" in s:
                rules["1-6"][1].append((rel, i, "含 TAB"))
            if len(s) > args.max_len:
                rules["1-9"][1].append((rel, i, "%d 列" % len(s)))
            if is_comment or is_directive:
                continue
            if RE_INLINE_BLOCK.match(s):
                rules["1-8"][1].append((rel, i, s.strip()))
            elif RE_NO_BRACE.match(s):
                rules["1-5"][1].append((rel, i, s.strip()))
            elif RE_TWO_STMT.search(s):
                rules["1-4"][1].append((rel, i, s.strip()))

    total = sum(len(v[1]) for v in rules.values())
    print("=" * 78)
    print("  华为 C 语言编程规范 · 自查（可自动化部分）")
    print("  扫描 %s ：%d 个文件 / %d 行" % (os.path.relpath(SRC_DIR, ROOT), n_files, n_lines))
    print("=" * 78)

    if total == 0:
        print("\n  ✔ 没有发现可自动判定的违规。")
        return 0

    for k in sorted(rules):
        name, items = rules[k]
        if not items:
            continue
        print("\n[%s] %s —— %d 处" % (k, name, len(items)))
        show = items if args.all else items[:10]
        for rel, ln, note in show:
            print("    %-42s :%-5d %s" % (rel, ln, note[:80]))
        if len(items) > len(show):
            print("    …… 还有 %d 处（加 --all 看全部）" % (len(items) - len(show)))

    print("\n" + "-" * 78)
    print("  合计 %d 处" % total)
    print("  ★ 建议：先修 [1-5]（缺大括号）——那是真 bug 风险；")
    print("     [1-8]/[1-9] 属于排版，看团队约定，个人项目可以不改。")

    if args.md:
        p = args.md if os.path.isabs(args.md) else os.path.join(ROOT, args.md)
        os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write("# 代码规范自查（华为 C 语言编程规范 · 可自动化部分）\n\n")
            f.write("- 扫描范围：`%s`（%d 个文件 / %d 行）\n" % (
                os.path.relpath(SRC_DIR, ROOT).replace("\\", "/"), n_files, n_lines))
            f.write("- 行长上限：%d\n- 合计问题：**%d 处**\n\n" % (args.max_len, total))
            for k in sorted(rules):
                name, items = rules[k], rules[k][1]
                if not items:
                    continue
                f.write("## [%s] %s —— %d 处\n\n" % (k, rules[k][0], len(items)))
                f.write("| 文件 | 行 | 说明 |\n|---|---|---|\n")
                for rel, ln, note in items:
                    f.write("| `%s` | %d | %s |\n" % (rel, ln, note.replace("|", "\\|")[:100]))
                f.write("\n")
        print("\n  已写入 %s" % os.path.relpath(p, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
