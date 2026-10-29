# -*- coding: utf-8 -*-
"""gen_ai_code.py —— 题2 第 ④ 步：把 ai/model/model.onnx 重新转成板端 C 代码

【为什么必须有这一步】
  重采数据 → 重训之后：
    * ai_norm.h / ai_test10.h  由 tools/train_nn.py **自动**写进 firmware/USER/AI/  ✔
    * firmware/USER/AI/network*.c  是 stedgeai 生成的，还是**上一版模型的权重**  ✘
  只跑 train_nn.py 不跑本脚本 ⇒ 板上 = 「新的归一化参数 + 旧的网络权重」，
  现象是"输出看着有数、其实全错"，而且很难查。**必须成对更新。**

【为什么不用 CubeMX 里的 GENERATE CODE】
  1) stedgeai 内部用 GBK 处理路径：模型路径/输出路径只要带中文就崩
     （`'gbk' codec can't encode characters...`）。本脚本在**纯英文临时目录**里跑。
  2) GENERATE CODE 还会顺手破坏 Keil 工程：把第一个 Target 改名成 firmware、
     给部分文件加"文件级 IncludePath"（AC5 报 C4065E）、删
     Drivers/.../Templates/system_stm32f4xx.c、把 startup 的 Stack_Size 改回 0x800。
  ⇒ 本脚本只**覆盖 firmware/USER/AI/ 里的网络代码**，不碰工程结构。

【用法】
    python tools/gen_ai_code.py                    # 自动找 stedgeai + 用 %USERPROFILE%\\lab7_ai
    python tools/gen_ai_code.py --work D:\\ai_work  # 指定英文工作目录
    python tools/gen_ai_code.py --stedgeai <exe>   # 手动指定 stedgeai.exe
    python tools/gen_ai_code.py --dry-run          # 只打印将要执行的命令

跑完接着做（脚本会再提示一遍）：
    python tools/make_project.py firmware
    cd firmware/MDK-ARM && UV4 -b firmware.uvprojx -t board1 -o build_b.log
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # Lab7/
FW_AI = os.path.join(ROOT, "firmware", "USER", "AI")
MODEL = os.path.join(ROOT, "ai", "model", "model.onnx")

# 从 out2 里搬回工程的产物（其余如 LICENSE.txt / *_info.json / *_report.txt 留作存档）
COPY_NAMES = ["network.c", "network.h", "network_config.h",
              "network_data.c", "network_data.h",
              "network_data_params.c", "network_data_params.h"]

_STEDGEAI_BASE = os.path.join("STM32Cube", "Repository", "Packs",
                              "STMicroelectronics", "X-CUBE-AI")
_STEDGEAI_TAIL = os.path.join("*", "Utilities", "windows", "stedgeai.exe")


def log(msg):
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


def find_stedgeai():
    """找 stedgeai.exe：环境变量 → 当前用户目录 → 逐盘扫。

    ★ 不要用 `?:\\...` 这种 pattern —— **glob 在 Windows 不认盘符通配**
      （实测 `?:\\Users\\*\\...` 返回 0 条，而 `D:\\Users\\*\\...` 正常），
      所以这里显式遍历盘符。找不到就让用户设 STEDGEAI 环境变量。
    """
    env = os.environ.get("STEDGEAI", "")
    if env and os.path.isfile(env):
        return env

    hits = []
    hits += glob.glob(os.path.join(os.path.expanduser("~"), _STEDGEAI_BASE, _STEDGEAI_TAIL))
    for drv in "CDEFGH":
        # <盘>:\Users\<任意用户>\STM32Cube\...
        hits += glob.glob("%s:\\Users\\*\\%s\\%s" % (drv, _STEDGEAI_BASE, _STEDGEAI_TAIL))
        # <盘>:\STM32Cube\...（有些装在盘根）
        hits += glob.glob("%s:\\%s\\%s" % (drv, _STEDGEAI_BASE, _STEDGEAI_TAIL))
    hits = sorted(set(hits), key=lambda p: p, reverse=True)   # 版本号大的优先
    return hits[0] if hits else None


def is_ascii_path(p):
    try:
        p.encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def run(cmd, cwd=None):
    log("  $ " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    # stedgeai 会问一句确认，喂个 n（与手工 `echo n | stedgeai ...` 等价）
    p = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out, _ = p.communicate(input=b"n\n\n")
    txt = out.decode("utf-8", "replace")
    for line in txt.splitlines():
        if line.strip():
            log("    | " + line.rstrip())
    return p.returncode, txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stedgeai", default=None, help="stedgeai.exe 路径（默认自动查找）")
    ap.add_argument("--work", default=None, help="英文工作目录（默认 %%USERPROFILE%%\\\\lab7_ai）")
    ap.add_argument("--name", default="network", help="生成的网络名（默认 network）")
    ap.add_argument("--dry-run", action="store_true", help="只打印命令，不执行")
    args = ap.parse_args()

    if not os.path.isfile(MODEL):
        log("!! 找不到模型 %s —— 先跑 python tools/train_nn.py" % MODEL)
        return 1

    stedgeai = args.stedgeai or find_stedgeai()
    if not stedgeai:
        log("!! 没找到 stedgeai.exe，请用 --stedgeai 指定")
        log("   （一般在这个 pack 里：...\\X-CUBE-AI\\<版本>\\Utilities\\windows\\stedgeai.exe）")
        return 1

    work = args.work or os.path.join(os.path.expanduser("~"), "lab7_ai")
    if not is_ascii_path(work) or not is_ascii_path(stedgeai):
        log("!! 工作目录 / stedgeai 路径含非 ASCII 字符 —— stedgeai 会崩，换纯英文路径")
        log("   work = %s" % work)
        return 1

    log("stedgeai : %s" % stedgeai)
    log("模型      : %s" % MODEL)
    log("工作目录  : %s   （纯英文，stedgeai 才不会崩）" % work)

    if not args.dry_run:
        os.makedirs(work, exist_ok=True)
        # 模型也复制一份到英文目录，避免 -m 指向中文路径
        shutil.copyfile(MODEL, os.path.join(work, "model.onnx"))

    out_analyze = os.path.join(work, "out")
    out_gen = os.path.join(work, "out2")

    # ★★ 不用 workspace（`--no-workspace`）。
    #   stedgeai 默认在 workspace 里建 `generated_network`，而 **generate 阶段开工前
    #   会先去删它**（哪怕是自己刚在 analyze 里建的）。这个删除在本机是
    #   间歇性失败的：`[safe-delete][SAFE_DELETE_FAIL_CLOSED] ... trash-failed` → 退出 -1。
    #   试过换唯一 workspace 目录，照样失败（同一次运行里 analyze→generate 都要删）。
    #   `--no-workspace` 直接不建那个目录，从根上绕开。
    for old in glob.glob(os.path.join(work, "ws_*")) + glob.glob(os.path.join(work, "st_ai_ws")):
        shutil.rmtree(old, ignore_errors=True)

    common = ["--target", "stm32f4", "--name", args.name,
              "-m", os.path.join(work, "model.onnx"),
              "--compression", "none",
              "--no-workspace"]

    log("\n[1/3] analyze —— 看 Flash / RAM / MACC")
    # ★ cwd 必须是 work（~/lab7_ai），不能让 stedgeai 在**工程目录**里建 st_ai_ws ——
    #   那个 workspace 会残留 generated_network，第二次跑时 stedgeai 删不掉它，
    #   就报 [safe-delete][SAFE_DELETE_FAIL_CLOSED] 然后退出 -1。
    #   表现出来就是"第一次能过、之后每次都失败"，极难查。
    rc, _ = (0, "") if args.dry_run else run(
        [stedgeai, "analyze"] + common + ["--output", out_analyze], cwd=work)
    if rc != 0:
        log("!! analyze 失败（exit=%d）" % rc)
        return rc

    log("\n[2/3] generate —— 生成 C 代码")
    rc, _ = (0, "") if args.dry_run else run(
        [stedgeai, "generate"] + common + ["--output", out_gen], cwd=work)
    if rc != 0:
        log("!! generate 失败（exit=%d）" % rc)
        return rc

    if args.dry_run:
        log("\n(dry-run 结束)")
        return 0

    log("\n[3/3] 覆盖 firmware/USER/AI/ 里的网络代码")
    changed, same, missing = [], [], []
    for name in COPY_NAMES:
        src = os.path.join(out_gen, name)
        dst = os.path.join(FW_AI, name)
        if not os.path.isfile(src):
            missing.append(name)
            continue
        old = open(dst, "rb").read() if os.path.isfile(dst) else None
        new = open(src, "rb").read()
        shutil.copyfile(src, dst)
        if old is None:
            changed.append(name + "  (新建)")
        elif old == new:
            same.append(name)
        else:
            changed.append(name)
    for n in changed:
        log("  ★ 已更新   %s" % n)
    for n in same:
        log("  = 无变化   %s" % n)
    for n in missing:
        log("  ! out2 里没有 %s（跳过）" % n)

    # 把 stedgeai 的报告一起存档，题3 文档要用
    rep_dir = os.path.join(ROOT, "ai", "report")
    if os.path.isdir(rep_dir):
        for fn, tgt in (("network_generate_report.txt", "stedgeai_generate_report.txt"),
                        ("network_c_info.json", "stedgeai_network_c_info.json")):
            src = os.path.join(out_gen, fn)
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(rep_dir, tgt))
                log("  ★ 报告存档   ai/report/%s" % tgt)

    log("\n完成。接着必须做这两步（否则板上跑的还是旧网络）：")
    log("  1) python tools/make_project.py firmware")
    log("  2) cd firmware/MDK-ARM && UV4 -b firmware.uvprojx -t board1 -o build_b.log   （两 Target 都要 0E0W）")
    log("  3) 重新烧写 MDK-ARM/board{1,2}/board{1,2}.hex")
    return 0


if __name__ == "__main__":
    sys.exit(main())
