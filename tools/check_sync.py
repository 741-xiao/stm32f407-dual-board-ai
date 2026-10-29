# -*- coding: utf-8 -*-
"""check_sync.py —— 检查「训练出来的模型」和「固件里的网络代码」是不是同一版

【为什么需要它】
  这个项目最容易翻车、且最难查的就是这一条：
      tools/train_nn.py      只更新  ai_norm.h / ai_test10.h   （归一化参数）
      tools/gen_ai_code.py   才更新  firmware/USER/AI/network*.c（**网络权重**）
  只跑前者不跑后者 ⇒ 板上 =「新的归一化 + 旧的权重」→ 输出看着有数、其实全错。
  已经栽过三次（uvprojx 回滚 / 并发跑挂掉 / 输出被管道吞掉），所以加个硬检查。

用法：
    python tools/check_sync.py            # 检查是否同步（退出码 0=同步，1=落后）
    python tools/check_sync.py --hex      # 顺便打印两个 hex 的 md5 / 时间（重烧前核对用）

判据：firmware/USER/AI/network_data_params.c（权重）的修改时间
      必须 **不早于** ai/model/model.onnx（模型）。
      再看一眼 ai_norm.h 也一并报出来。
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FW_AI = os.path.join(ROOT, "firmware", "USER", "AI")

MODEL = os.path.join(ROOT, "ai", "model", "model.onnx")
NORM = os.path.join(FW_AI, "ai_norm.h")
WEIGHT = os.path.join(FW_AI, "network_data_params.c")
HEXES = [os.path.join(ROOT, "firmware", "MDK-ARM", "board%d" % b, "board%d.hex" % b)
         for b in (1, 2)]


def mt(p):
    return os.path.getmtime(p) if os.path.isfile(p) else None


def fmt(p):
    t = mt(p)
    if t is None:
        return "(缺)"
    import datetime
    return datetime.datetime.fromtimestamp(t).strftime("%m-%d %H:%M:%S")


def md5(p):
    if not os.path.isfile(p):
        return "(缺)"
    return hashlib.md5(open(p, "rb").read()).hexdigest()[:12]


def main():
    want_hex = "--hex" in sys.argv
    print("=" * 70)
    print("  模型 / 固件 同步检查")
    print("=" * 70)
    miss = [p for p in (MODEL, WEIGHT) if not os.path.isfile(p)]
    if miss:
        print("✘ 缺少文件：")
        for p in miss:
            print("   " + os.path.relpath(p, ROOT))
        return 1

    print("  ai/model/model.onnx                          %s" % fmt(MODEL))
    print("  firmware/USER/AI/ai_norm.h                   %s   %s"
          % (fmt(NORM), "✓ 与模型同批" if mt(NORM) and mt(NORM) >= mt(MODEL) else "⚠ 比模型旧"))
    print("  firmware/USER/AI/network_data_params.c       %s   （网络权重）" % fmt(WEIGHT))

    stale = mt(WEIGHT) < mt(MODEL)
    if stale:
        print("\n✘ **固件里的网络权重比模型旧** —— 板上跑的是「新归一化 + 旧权重」：")
        print("   python tools/gen_ai_code.py          # ① 重转 network*.c")
        print("   python tools/make_project.py firmware # ②（一般不用，文件已挂好）")
        print("   编译 board1/board2（都要 0E0W）→ 重新烧写两个 hex")
    else:
        print("\n✔ 网络权重不早于模型 —— 若刚重编译过，直接烧 hex 即可。")

    if want_hex:
        print("\n  --- 待烧写的 hex（烧之前核对时间是不是刚编译的）---")
        for h in HEXES:
            print("  %-46s %s  md5=%s" % (os.path.relpath(h, ROOT), fmt(h), md5(h)))

    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
