#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""retrain.py —— 一条命令走完实验七题2 的「重采 → 重训 → 上板前」全流程

原来要手工跑 7 步，其中第 4 步（转板端 C）**最容易漏**，漏了板上就是
「新归一化 + 旧权重」—— 输出看着有数、其实全错，而且极难排查（这个坑栽过三次）。

    [1/7] 导出数据集       tools/export_dataset.py --min-per-board N
    [2/7] 可分性前置关     tools/check_separability.py <csv> --thr 0.80
    [3/7] 训练             tools/train_nn.py <csv>
    [4/7] 转板端 C 代码    tools/gen_ai_code.py          ← ★ 本脚本存在的最大理由
    [5/7] 更新 Keil 工程   tools/make_project.py firmware
    [6/7] 编译两 Target    UV4 -b（--build 才做）
    [7/7] 同步自检         tools/check_sync.py --hex

用法：
    python tools\\retrain.py                    # 全套（到第 7 步，不编译）
    python tools\\retrain.py --build            # 连编译一起（要装 Keil）
    python tools\\retrain.py --skip-export      # 数据已经导好，从第 2 步开始
    python tools\\retrain.py --skip-sep         # 跳过可分性关卡（数据明显够、赶时间）
    python tools\\retrain.py --min-per-board 120 --epochs 400

★ 为什么必须用受管 venv 跑：
    训练依赖 numpy / scikit-learn / tensorflow / onnx，系统 python 没有。
    C:\\Users\\<你>\\.workbuddy\\binaries\\python\\envs\\default\\Scripts\\python.exe

★ 训练**不可复现**：同样 --seed 两次跑出的权重都不同（实测），
  所以每次 train 之后都必须走第 4 步 + 重编译重烧，不能靠"文件看着没变"跳过。

★ 第 4 步的偶发失败（实测过一次 exit=-1）：
    stedgeai 的 --output 目录（~/lab7_ai/out、out2）**不会被它自己清空**，
    残留会让它偶发报错。本脚本在跑之前先清这两个目录，失败还会再重试一次。
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # Lab7/
TOOLS = os.path.join(ROOT, "tools")
PY = sys.executable
CSV = os.path.join("ai", "dataset", "lab7.csv")
STEDGE_WORK = os.path.join(os.path.expanduser("~"), "lab7_ai")       # gen_ai_code.py 的 --work 默认值


def find_uv4():
    """找 UV4.exe：环境变量 → 常见安装路径 → PATH。找不到返回空串。

    ★ 不写死 `D:\\Keil_v5\\...` —— 那样只打包 Lab7 换台机器就跑不了。
    """
    env = os.environ.get("UV4_PATH", "")
    if env and os.path.isfile(env):
        return env
    for drv in ("C", "D", "E", "F"):
        for base in ("Keil_v5", "Keil", "MDK", "Keil_v4"):
            p = "%s:\\%s\\UV4\\UV4.exe" % (drv, base)
            if os.path.isfile(p):
                return p
    return shutil.which("UV4") or ""


UV4 = find_uv4()


# ======================================================================
#  环境自检（--check）
#  ★ 换台电脑第一件事就是跑它：把「工程在哪 / 用哪个 python / 缺什么包 /
#    Keil 在哪 / stedgeai 在哪 / 库里有几条」一次说清楚，别等训练到一半才报错。
# ======================================================================

def _has(mod):
    """模块装没装 —— 只查 spec，不真 import（tensorflow 首次 import 要好几秒）。"""
    import importlib.util
    try:
        return importlib.util.find_spec(mod) is not None
    except Exception:
        return False


def _is_ascii(p):
    try:
        p.encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def find_stedgeai():
    """复用 gen_ai_code.py 的查找逻辑（环境变量 → 用户目录 → 逐盘扫）。"""
    if TOOLS not in sys.path:
        sys.path.insert(0, TOOLS)
    try:
        import gen_ai_code
        return gen_ai_code.find_stedgeai() or ""
    except Exception:
        return ""


def db_probe():
    """只读探一次库，返回 (状态, 说明)；状态 ok / err / miss。最多等 3 秒。"""
    if not _has("pymysql"):
        return "miss", "没装 pymysql（pip install pymysql）"
    try:
        pc = os.path.join(ROOT, "pc")
        if pc not in sys.path:
            sys.path.insert(0, pc)
        import db_local as d
        import pymysql
        conn = pymysql.connect(host=d.DB_HOST, port=int(d.DB_PORT), user=d.DB_USER,
                               password=d.DB_PASS, database=d.DB_NAME, connect_timeout=3)
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT board, COUNT(*) FROM lab7_data GROUP BY board")
                rows = dict(cur.fetchall())
        finally:
            conn.close()
        return "ok", "127.0.0.1:3306 / %s.lab7_data   板1 %d / 板2 %d 条" % (
            d.DB_NAME, rows.get(1, 0), rows.get(2, 0))
    except Exception as e:
        msg = str(e)
        if "1146" in msg:
            return "err", "表 lab7_data 不存在 —— 先跑 python tools/init_db.py"
        if "1049" in msg:
            return "err", "库 stm32_lab 不存在 —— 先跑 python tools/init_db.py"
        if "2003" in msg or "refused" in msg.lower():
            return "err", "MySQL 没在跑（127.0.0.1:3306 连不上）"
        return "err", msg.split("\n")[0][:110]


def csv_probe(path=CSV):
    """数一下 CSV 里两板各多少条；读不到返回 None。"""
    p = path if os.path.isabs(path) else os.path.join(ROOT, path)
    if not os.path.isfile(p):
        return None
    n = {1: 0, 2: 0}
    try:
        import csv as _csv
        with open(p, newline="", encoding="utf-8") as f:
            for r in _csv.DictReader(f):
                try:
                    b = int(r["board"])
                except Exception:
                    continue
                if b in n:
                    n[b] += 1
        return n
    except Exception:
        return None


def print_check():
    ok = lambda b: "✔" if b else "✘"          # noqa: E731
    hr("=")
    print("  实验七 题2 · 运行环境自检")
    hr("=")
    print("  工程目录 : %s" % ROOT)
    print("             路径含中文：%s（Python 不受影响；转 C 会自动用 %s）"
          % ("是" if not _is_ascii(ROOT) else "否", STEDGE_WORK))
    print("  Python   : %s  (%s)" % (sys.executable, platform.python_version()))

    print("  依赖     :")
    deps = [("numpy", "数据处理", True), ("tensorflow", "训练", True),
            ("onnx", "导出 model.onnx", True), ("matplotlib", "画图（可缺）", False),
            ("sklearn", "可分性检验（--skip-sep 可缺）", False),
            ("pymysql", "读 MySQL（--skip-export 可缺）", False)]
    missing = []
    for m, why, _need in deps:
        good = _has(m)
        if not good:
            missing.append(m)
        print("             %s %-11s %s" % (ok(good), m, why))

    print("  Keil UV4 : %s %s" % (ok(bool(UV4)), UV4 or "没找到（--build 会跳过）"))
    st = find_stedgeai()
    print("  stedgeai : %s %s" % (ok(bool(st)), st or "没找到（转 C 这步过不去）"))
    state, note = db_probe()
    print("  MySQL    : %s %s" % (ok(state == "ok"), note))
    n = csv_probe()
    if n:
        tip = "" if min(n[1], n[2]) >= 100 else "   ← 题面要求各 ≥100"
        print("  数据集   : ✔ %s   板1 %d / 板2 %d%s" % (CSV, n[1], n[2], tip))
    else:
        print("  数据集   : ✘ %s 不存在或读不出" % CSV)
    hr("=")

    if missing:
        print("  缺 %d 个包，装一次就好（只装进上面这个 venv / 解释器，不动系统）：" % len(missing))
        print('    "%s" -m pip install %s' % (sys.executable, " ".join(missing)))
    else:
        print("  训练所需依赖齐全。")
    if not UV4:
        print("  Keil 没找到：设环境变量 UV4_PATH=<...\\UV4.exe>，或去掉 --build 手动 F7。")
    if not st:
        print("  stedgeai 没找到：CubeMX → Manage embedded software packages →")
        print("  X-CUBE-AI 10.2.1 重新勾选安装（缺的是 Utilities\\windows\\stedgeai.exe）。")
    if state != "ok":
        print("  数据库不可用：不重采就加 --skip-export，直接用现有 CSV。")
    hr("=")

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


class StepFailed(Exception):
    def __init__(self, step, code, secs):
        self.step, self.code, self.secs = step, code, secs
        super().__init__("step %s exit=%d" % (step, code))


def hr(ch="─", n=78):
    print(ch * n)


def clean_stedgeai_out():
    """清掉 stedgeai 的输出目录 —— 残留会让它偶发 exit=-1。

    ★★ 关键是 `st_ai_ws`：stedgeai 每次开工都先去删上一次的
    `st_ai_ws/generated_network`，那个删除是**间歇性失败**的
    （报 `[safe-delete][SAFE_DELETE_FAIL_CLOSED] ... trash-failed` 然后退出 -1）。
    表现出来极迷惑：同一个脚本单独跑成功、在 retrain 里跑失败，重试也失败。
    既然它删不掉旧的，那我们替它**整个删掉 workspace**，让它每次都是全新的。
    """
    for d in ("out", "out2", "st_ai_ws"):
        p = os.path.join(STEDGE_WORK, d)
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
            if os.path.isdir(p):
                print("   ⚠ 清不掉 %s（可能有进程占用），转 C 可能失败" % p)


def run(step, title, cmd, cwd=None, extra_note="", check_rc=True):
    hr()
    print("  [%s] %s" % (step, title))
    if extra_note:
        print("       %s" % extra_note)
    hr()
    t0 = time.time()
    # ★ 编译步骤必须 cd 到 firmware\MDK-ARM 再跑 UV4。
    #   工程文件在那儿，UV4 不认"相对到 Lab7 根"的 firmware.uvprojx
    #   （会弹 "Cannot read project file ...\Lab7\firmware.uvprojx"）。
    #   另外 -o 的日志也落在这个目录，check 的时候才读得到刚生成的那份。
    if step == "6/7":
        cwd = os.path.join(ROOT, "firmware", "MDK-ARM")
    # ★ 关键：强制子进程用 UTF-8 输出。
    #   gen_ai_code.py 里有大量中文和 ✔ ★ ─ 之类的符号；如果子进程的 stdout
    #   落到 GBK（重定向到文件时就是这种情况），print 会抛 UnicodeEncodeError，
    #   而异常发生在 flush 之前 ⇒ **输出全丢 + 退出码 -1**，看起来像"stedgeai 失败"。
    #   实测踩过：同一个脚本单独跑成功、在 retrain 里跑必崩。
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    # 捕获再打印：子进程全缓冲，崩溃时缓冲会丢 —— 直接继承的话什么都看不到。
    # 捕获后至少能拿到它崩之前写出的内容。
    p = subprocess.run(cmd, cwd=cwd or ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    if p.stdout:
        sys.stdout.write(p.stdout)
        sys.stdout.flush()
    dt = time.time() - t0
    if p.returncode != 0:
        if check_rc:
            print("\n✘ [%s] 失败（退出码 %d，耗时 %.1fs）" % (step, p.returncode, dt))
            raise StepFailed(step, p.returncode, dt)
        # ★ UV4 的返回码不可靠：编译成功（0 Error 0 Warning + axf 生成）时也可能返回 15
        #   （"目标已是最新"之类的状态码）。编译步骤传 check_rc=False，改看日志。
        print("\n   （该步骤返回 %d —— 已知 UV4 的返回码不可靠，下面按日志判定）" % p.returncode)
    print("\n✔ [%s] 完成（%.1fs）" % (step, dt))
    return dt


def parse_keil_log(path):
    """从 Keil 的 build log 里抠出 Program Size / Error / Warning。"""
    if not os.path.exists(path):
        return None
    info = {}
    with open(path, "r", encoding="gbk", errors="replace") as f:
        for ln in f:
            if "Program Size:" in ln:
                info["size"] = ln.strip()
            if "Error(s)" in ln and "Warning(s)" in ln:
                info["result"] = ln.strip()
    return info or None


def main():
    ap = argparse.ArgumentParser(description="实验七 题2 一键重训（重采→重训→上板前）")
    ap.add_argument("--min-per-board", type=int, default=100, help="导出门槛：每板至少多少条")
    ap.add_argument("--thr", type=float, default=0.80, help="可分性门槛（默认 0.80）")
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--hidden", default="16,16")
    ap.add_argument("--holdout-per-board", type=int, default=5)
    ap.add_argument("--skip-export", action="store_true", help="跳过导出（直接用现有 csv）")
    ap.add_argument("--skip-sep", action="store_true", help="跳过可分性关卡")
    ap.add_argument("--build", action="store_true", help="顺便编译 board1/board2")
    ap.add_argument("--fast", action="store_true",
                    help="快模式：epochs 150 + 跳过分可性（每次环境都变、每次都要重采时用）")
    ap.add_argument("--yes", action="store_true", help="不做任何确认")
    ap.add_argument("--check", action="store_true",
                    help="只做环境自检（路径/依赖/Keil/stedgeai/MySQL/数据集），不训练不改文件")
    args = ap.parse_args()

    if args.check:
        print_check()
        return 0

    # --fast：给"每次测试环境都不一样、数据每次都要重采"的场景。
    #   实测 300 epoch 训练 32 s、150 epoch 约 16 s；而数据本身是几十 σ 分得开的，
    #   少训一半对准确率没有可测影响。
    #   同时跳过分可性关卡 —— 那是"第一次确认两板能不能分开"用的，
    #   环境每次变的话它只会拖时间。
    if args.fast:
        args.epochs = min(args.epochs, 150)
        args.skip_sep = True
        # ★ 不再卡「每板 100 条」的门槛：环境每次变、每次重采时，有多少用多少。
        #   train_nn.py 里仍有「每板 ≥20 条」的底线兜着。
        #   题面要 100 条 —— 正式采集/交作业时别加 --fast。
        args.min_per_board = 0

    os.chdir(ROOT)

    print()
    hr("=")
    print("  实验七 题2 · 一键重训")
    print("  每板 ≥%d 条 → 训练 %d epoch → 转 C → 进工程%s → 同步自检"
          % (args.min_per_board, args.epochs, " → 编译" if args.build else ""))
    print("  工程：%s" % ROOT)
    hr("=")

    # ---- 依赖前置关：缺包就直说，别等训练到一半抛 traceback ----
    #   numpy/tensorflow/onnx 是训练+导出必需的；sklearn 只有可分性要用；pymysql 只有导出要用。
    need = ["numpy", "tensorflow", "onnx"]
    if not args.skip_sep:
        need.append("sklearn")
    if not args.skip_export:
        need.append("pymysql")
    missing = [m for m in need if not _has(m)]
    if missing:
        print("\n✘ 当前解释器缺依赖：%s" % "、".join(missing))
        print("  解释器：%s" % sys.executable)
        print("  装一次就好（只装进这个解释器，不动系统）：")
        print('    "%s" -m pip install %s' % (sys.executable, " ".join(missing)))
        print("  想看完整环境报告：python tools/retrain.py --check")
        return 3

    if not args.yes:
        try:
            a = input("\n开始？(直接回车继续，输入 n 取消)：").strip().lower()
        except EOFError:
            a = ""
        if a == "n":
            print("已取消。")
            return 0

    steps = []
    total_t0 = time.time()

    # ---- 1 导出 ----
    if not args.skip_export:
        steps.append(("1/7", "导出数据集（lab7_data → CSV）",
                      [PY, "tools/export_dataset.py", "--out", CSV,
                       "--min-per-board", str(args.min_per_board)], ""))
    else:
        print("\n· 跳过导出（--skip-export），直接用 %s" % CSV)

    # ---- 2 可分性 ----
    if not args.skip_sep:
        steps.append(("2/7", "可分性前置关（< %.0f%% 就该先换摆放再重采）" % (args.thr * 100),
                      [PY, "tools/check_separability.py", CSV, "--thr", str(args.thr)], ""))
    else:
        print("· 跳过分可性关卡（--skip-sep）")

    # ---- 3 训练 ----
    steps.append(("3/7", "训练神经网络（导出 ai_norm.h / ai_test10.h / model.onnx）",
                  [PY, "tools/train_nn.py", CSV,
                   "--epochs", str(args.epochs),
                   "--hidden", args.hidden,
                   "--holdout-per-board", str(args.holdout_per_board)],
                  "★ 训练不可复现：跑完必须接着走第 4 步"))

    # ---- 4 转 C（强制，不可跳过）----
    steps.append(("4/7", "转板端 C 代码（network*.c）—— ★ 最容易漏的一步",
                  [PY, "tools/gen_ai_code.py"],
                  "漏了这步 = 板上「新归一化 + 旧权重」，看着有数其实全错"))

    # ---- 5 进工程 ----
    steps.append(("5/7", "更新 Keil 工程（确保 AI 文件组 + 两个 Target 都在）",
                  [PY, "tools/make_project.py", "firmware"], ""))

    # ---- 6 编译 ----
    if args.build:
        if not UV4:
            print("\n✘ 找不到 Keil UV4.exe")
            print("  三种办法：① 设环境变量 UV4_PATH=<...\\UV4.exe>")
            print("            ② 装到默认位置 C:\\Keil_v5 或 D:\\Keil_v5")
            print("            ③ 不加 --build，在 Keil 里手动 F7")
            return 2
        steps.append(("6/7", "编译 board1（0 Error 0 Warning）",
                      [UV4, "-b", "firmware.uvprojx", "-t", "board1", "-o", "build_board1.log"], ""))
        steps.append(("6/7", "编译 board2",
                      [UV4, "-b", "firmware.uvprojx", "-t", "board2", "-o", "build_board2.log"], ""))
    else:
        print("· 跳过编译（加 --build 或到 Keil 里 F7）")

    # ---- 7 同步自检 ----
    steps.append(("7/7", "模型 / 固件 同步自检 + hex md5",
                  [PY, "tools/check_sync.py", "--hex"], ""))

    # ---- 执行 ----
    for step, title, cmd, note in steps:
        if step == "4/7":
            # stedgeai 的输出目录不清会偶发 exit=-1（实测过一次）
            clean_stedgeai_out()
        try:
            run(step, title, cmd, extra_note=note, check_rc=(step != "6/7"))
        except StepFailed as e:
            if step == "4/7":
                print("\n⚠ 转 C 失败（exit=%d）。已清空 stedgeai 输出目录，重试一次…" % e.code)
                clean_stedgeai_out()
                run(step, title + "（重试）", cmd, extra_note=note)
            elif step == "1/7":
                print("\n⚠ 导出失败。常见原因：库里没有 lab7_data 表 / MySQL 没启动 / 没装 pymysql。")
                print("  不重采的话加 --skip-export，直接用现有 %s 继续。" % CSV)
                print("  查环境：python tools/retrain.py --check")
                raise SystemExit(e.code)
            else:
                raise SystemExit(e.code)

        # 编译步骤的结论在日志里，不在 stdout
        if step == "6/7" and "-o" in cmd:
            logname = cmd[cmd.index("-o") + 1]
            info = parse_keil_log(os.path.join(ROOT, "firmware", "MDK-ARM", logname))
            if info:
                if "size" in info:
                    print("   %s" % info["size"])
                if "result" in info:
                    print("   %s" % info["result"])
                    # ★ 编译成败**只看日志**：UV4 成功时也可能返回 15
                    if "0 Error(s)" not in info["result"]:
                        print("   ✘ 编译有错误，看 firmware\\MDK-ARM\\%s" % logname)
                        raise SystemExit(1)
            else:
                print("   ⚠ 没找到 %s，无法判定编译结果" % logname)

        # 导完就报一下实际条数 —— 免得"只有 37 条"这种事跑完才知道
        if step == "1/7":
            n = csv_probe()
            if n:
                tip = ""
                if min(n[1], n[2]) < 100:
                    tip = "   ← 少于题面的 100 条（--fast 模式不拦截）"
                print("   数据集：板1 %d 条 / 板2 %d 条%s" % (n[1], n[2], tip))

    # ---- 收尾 ----
    dt = time.time() - total_t0
    hr("=")
    print("  ✔ 全流程完成，总耗时 %.1f 分" % (dt / 60.0))
    hr("=")
    print("  下一步：")
    if args.build:
        print("    1. 把 firmware\\MDK-ARM\\board{1,2}\\board{1,2}.hex 烧进两块板")
        print("    2. 两板上电（先板2 再板1）")
    else:
        print("    1. Keil 里 F7 编译两个 Target")
        print("    2. 烧 hex")
    print("    3. ★ 采集时的**摆放**必须和演示时完全一样（10cm + 传感器朝向错开）")
    print("    4. 板上串口敲 n 跑一次 ai_test10.h 的 10 条留出样本，确认判定正确")
    hr("=")
    return 0


if __name__ == "__main__":
    sys.exit(main())
