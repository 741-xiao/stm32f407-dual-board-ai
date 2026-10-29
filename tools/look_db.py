#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一眼看清"现在手里有什么数据" —— 库内现状 + 通道统计 + 板上 AI 判定。

为什么单独一个脚本：题2 的每一步（导出 / 可分性 / 训练 / 答辩 / 写报告）都要先回答
"库里现在有多少条、两板分不分得开、板上 AI 判得准不准"。以前每次都得临时写 SQL，
口径还容易不一致；固化成一个脚本，输出就是**统一口径**的数据快照。

用法：
    python tools/look_db.py                 # 库内现状 + 通道统计 + AI 混淆矩阵 + CSV 快照
    python tools/look_db.py --recent 20     # 顺便打印最近 20 条原始记录
    python tools/look_db.py --no-db         # 不连库，只看 ai/dataset/*.csv

★ 只读：不写库、不改任何文件（要改数据请用 export_dataset.py / 上位机）。
"""

import argparse
import csv
import datetime
import glob
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:                                  # 老 python 没有这个属性
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FEATS = ["temp", "humi", "light", "eco2", "tvoc"]
CN = {"temp": "温度℃", "humi": "湿度%RH", "light": "光敏",
      "eco2": "CO2ppm", "tvoc": "TVOCppb"}


def sep(cn, n=72):
    print("=" * n)
    print("  " + cn)
    print("=" * n)


def rel_sigma(a, b):
    """相对差 = |Δ均值| / ((σ1+σ2)/2)。与 check_separability.py 同一口径。"""
    sd = (a.std() + b.std()) / 2.0 or 1e-9
    return abs(a.mean() - b.mean()) / sd


# ----------------------------------------------------------------- 库内现状

def show_db(recent):
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    sys.path.insert(0, os.path.join(ROOT, "pc"))
    try:
        import numpy as np
        from export_dataset import load_db_conf
        import pymysql
    except ImportError as e:
        print("✘ 缺依赖（%s）—— 用受管 venv 跑：\n"
              "    C:\\Users\\<u>\\.workbuddy\\binaries\\python\\envs\\default\\Scripts\\python.exe "
              "tools/look_db.py" % e)
        return False

    conf = load_db_conf()
    if conf is None:
        print("✘ 没找到数据库配置（pc/db_local.py 或 STM32_DB_* 环境变量）")
        return False
    try:
        conn = pymysql.connect(charset="utf8mb4", autocommit=True, **conf)
    except Exception as e:
        print("✘ 连不上 MySQL：%s" % e)
        return False
    cur = conn.cursor()

    sep("A. 库内现状   %s.lab7_data" % conf.get("database", "stm32_lab"))
    cur.execute("SELECT board, COUNT(*), MIN(id), MAX(id), MIN(recv_time), MAX(recv_time) "
                "FROM lab7_data GROUP BY board ORDER BY board")
    got = cur.fetchall()
    if not got:
        print("  （表是空的 —— 还没采过数据，或刚被清表）")
    for b, n, i0, i1, t0, t1 in got:
        print("  板%d  %4d 条   id %d~%d   %s ~ %s" % (b, n, i0, i1, t0, t1))

    cur.execute("SELECT board, status, COUNT(*) FROM lab7_data "
                "GROUP BY board, status ORDER BY board, status")
    st_rows = cur.fetchall()
    for b, st, n in st_rows:
        print("        └ 板%d  %-4s %4d 条" % (b, st, n))
    if any(st == "ALM" and n >= 20 for _, st, n in st_rows):
        print("  ★ 有大量 ALM：采**训练数据**前请在板子串口敲 `a 35` 把报警阈值调高"
              "（ALM 行默认被 export_dataset.py 剔除）")

    # ---- 通道统计（全量，不剔 ALM）----
    cur.execute("SELECT board,temp,humi,light,eco2,tvoc FROM lab7_data ORDER BY id")
    rows = cur.fetchall()
    if rows:
        b = np.array([r[0] for r in rows])
        X = np.array([[r[1], r[2], r[3], r[4], r[5]] for r in rows], dtype=float)
        m1, m2 = (b == 1), (b == 2)
        print("\n  【通道统计·全量】相对差 = |Δ均值| / ((σ1+σ2)/2)，>1 说明两簇基本分开")
        print("  %-10s %-20s %-20s %s" % ("通道", "板1 均值±σ", "板2 均值±σ", "相对差"))
        for i, k in enumerate(FEATS):
            if m1.sum() and m2.sum():
                a, c = X[m1, i], X[m2, i]
                print("  %-10s %-20s %-20s %.2fσ" % (
                    CN[k], "%.1f ± %.1f" % (a.mean(), a.std()),
                    "%.1f ± %.1f" % (c.mean(), c.std()), rel_sigma(a, c)))
        print("  ★ 温度 σ 往往≈0（SHT30 稳定后读数恒定）—— 此时 σ 是「除以极小分母」，"
              "实质看两板均值差")

    # ---- 板上 AI 判定 ----
    cur.execute("SELECT COUNT(*), SUM(ai_cls IS NULL), SUM(ai_cls=0) FROM lab7_data")
    tot, null_n, zero_n = cur.fetchone()
    if tot:
        cur.execute("SELECT board, ai_cls, COUNT(*) FROM lab7_data "
                    "GROUP BY board, ai_cls ORDER BY board, ai_cls")
        cm = {}
        for bb, aa, n in cur.fetchall():
            cm[(bb, aa)] = int(n)
        print("\n  【板上 AI 判定】ai_cls 为 NULL %s · =0（未推理）%s"
              % (null_n or 0, zero_n or 0))
        print("               ┌──────── 判定结果（ai_cls）────────┐")
        print("    真实板号   |  0 未推理    1 判为板1    2 判为板2  |   合计")
        print("    " + "-" * 58)
        for bb in (1, 2):
            r = [cm.get((bb, a), 0) for a in (0, 1, 2)]
            print("      板%d      |  %7d  %10d  %10d   |  %5d" % (bb, r[0], r[1], r[2], sum(r)))
        ok = sum(cm.get((bb, bb), 0) for bb in (1, 2))
        bad = sum(cm.get((bb, a), 0) for bb in (1, 2) for a in (1, 2) if a != bb)
        if ok + bad:
            print("\n    判对 %d / 判错 %d  ⇒  准确率 %.4f（%.2f%%）"
                  % (ok, bad, ok / (ok + bad), 100.0 * ok / (ok + bad)))
        cur.execute("SELECT board, MIN(ai_conf), AVG(ai_conf), MAX(ai_conf) FROM lab7_data "
                    "WHERE ai_cls IN (1,2) GROUP BY board ORDER BY board")
        cf = cur.fetchall()
        if cf:
            print("    置信度 ai_conf："
                  + " · ".join("板%d %s~%s(avg %.1f)" % (bb, mn, mx, av) for bb, mn, av, mx in cf))
        cur.execute("SELECT id,board,ai_cls,ai_conf,temp,humi,light,eco2,tvoc "
                    "FROM lab7_data WHERE ai_cls IN (1,2) AND ai_cls <> board ORDER BY id")
        wrong = cur.fetchall()
        if wrong:
            print("    ★ 判错的样本（先看是不是传感器预热/异常值，别急着怀疑模型）：")
            for r in wrong:
                print("      id=%s 真板%d→判%d conf=%s | T%.1f H%.1f L%d C%d V%d" % r)

    # ---- 最近 N 条 ----
    if recent > 0:
        cur.execute("SELECT id,board,seq,temp,humi,light,eco2,tvoc,status,ai_cls,ai_conf "
                    "FROM lab7_data ORDER BY id DESC LIMIT %d" % recent)
        print("\n  【最近 %d 条原始记录】" % recent)
        print("  %-5s %-4s %-6s %-7s %-7s %-6s %-7s %-6s %-5s %-6s %s"
              % ("id", "板", "seq", "温度", "湿度", "光敏", "CO2", "TVOC", "状态", "AI类", "置信%"))
        for r in cur.fetchall():
            print("  %-5s %-4s %-6s %-7s %-7s %-6s %-7s %-6s %-5s %-6s %s" % (
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
                r[9] if r[9] is not None else "-", r[10] if r[10] is not None else "-"))
    conn.close()
    return True


# ------------------------------------------------------------- CSV 快照

def show_csv():
    sep("B. CSV 快照   ai/dataset/*.csv")
    paths = sorted(glob.glob(os.path.join(ROOT, "ai", "dataset", "*.csv")))
    if not paths:
        print("  （还没有快照 —— 先跑 tools/export_dataset.py）")
        return
    try:
        import numpy as np
    except ImportError:
        np = None
    for p in paths:
        with open(p, encoding="utf-8") as f:
            rec = list(csv.DictReader(f))
        n1 = sum(1 for r in rec if r.get("board") == "1")
        n2 = sum(1 for r in rec if r.get("board") == "2")
        alm = sum(1 for r in rec if str(r.get("status", "")).upper() == "ALM")
        ai = sum(1 for r in rec if str(r.get("ai_cls", "0")) not in ("", "0", "None"))
        print("\n  %s  （%s）" % (os.path.basename(p), datetime.datetime.fromtimestamp(
            os.path.getmtime(p)).strftime("%m-%d %H:%M:%S")))
        print("    共 %d 条：板1 %d / 板2 %d ；ALM %d 条 ；带 ai_cls %d 条"
              % (len(rec), n1, n2, alm, ai))
        if np is None or not rec:
            continue
        def col(bd):
            return np.array([[float(r["temp"]), float(r["humi"]), float(r["light"]),
                              float(r["eco2"]), float(r["tvoc"])]
                             for r in rec if r.get("board") == bd], dtype=float)
        y1, y2 = col("1"), col("2")
        if len(y1) and len(y2):
            for i, k in enumerate(FEATS):
                print("      %-10s 板1 %7.1f±%-6.1f 板2 %7.1f±%-6.1f  %.2fσ"
                      % (CN[k], y1[:, i].mean(), y1[:, i].std(),
                         y2[:, i].mean(), y2[:, i].std(), rel_sigma(y1[:, i], y2[:, i])))


def main():
    ap = argparse.ArgumentParser(description="只读查看实验七数据现状")
    ap.add_argument("--recent", type=int, default=6, help="打印最近 N 条原始记录（0=不打印）")
    ap.add_argument("--no-db", action="store_true", help="跳过 MySQL，只看 CSV 快照")
    args = ap.parse_args()

    if not args.no_db:
        show_db(args.recent)
    show_csv()
    print("\n（只读快照，未改动任何数据。要导出数据集 → tools/export_dataset.py）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
