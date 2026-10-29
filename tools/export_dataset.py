#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 MySQL `stm32_lab.lab7_data` 导出题2 用的数据集（CSV）。

为什么单独一个脚本：题2 要求"两板各 ≥100 条"，训练/可分性检验/答辩都要一个
**固定口径**的数据快照。直接从库里 SELECT 出来落成 CSV，后面所有脚本都读这个 CSV，
就不会出现"这次跑用 A 口径、下次用 B 口径"的扯皮。

用法：
    # 1) 默认导出到 ai/dataset/lab7.csv（目录不存在会自动建）
    python tools/export_dataset.py

    # 2) 指定输出 / 只要某板 / 限制条数
    python tools/export_dataset.py --out ai/dataset/run2.csv --board 2 --limit 500

    # 3) 导出后立刻检查"两板各 ≥100 条"（不够就报错退出码 1，方便接流水线）
    python tools/export_dataset.py --min-per-board 100

数据来源与口令：与上位机同一套约定 —— 优先读 `pc/db_local.py`，没有就取环境变量
    STM32_DB_HOST / STM32_DB_PORT / STM32_DB_USER / STM32_DB_PASS / STM32_DB_NAME
（这样不用把密码写进任何入库的脚本里。）

输出的 CSV 列（固定，别改顺序 —— 后面三个脚本都按它解析）：
    board,seq,up_ms,temp,humi,light,eco2,tvoc,status,sample_time,recv_time,ai_cls,ai_conf
  其中 sample_time 是"采集时间"（PC 按每板 up_ms 换算），status 只留 OK 行
  （ALM = 板1 超温报警期，训练时算异常工况，默认剔除；要留加 --keep-alm）。
"""

import argparse
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COLS = ["board", "seq", "up_ms", "temp", "humi", "light", "eco2", "tvoc",
        "status", "sample_time", "recv_time", "ai_cls", "ai_conf"]


def load_db_conf():
    """优先 pc/db_local.py；退回环境变量。返回 dict 或 None。"""
    pc = os.path.join(ROOT, "pc")
    if pc not in sys.path:
        sys.path.insert(0, pc)
    try:
        import db_local as d           # noqa: E402
        return dict(host=d.DB_HOST, port=int(d.DB_PORT), user=d.DB_USER,
                    password=d.DB_PASS, database=d.DB_NAME)
    except Exception:
        pass
    if not os.environ.get("STM32_DB_USER"):
        return None
    return dict(host=os.environ.get("STM32_DB_HOST", "127.0.0.1"),
                port=int(os.environ.get("STM32_DB_PORT", "3306")),
                user=os.environ["STM32_DB_USER"],
                password=os.environ.get("STM32_DB_PASS", ""),
                database=os.environ.get("STM32_DB_NAME", "stm32_lab"))


def main():
    ap = argparse.ArgumentParser(description="导出实验七数据集（lab7_data → CSV）")
    ap.add_argument("--out", default=os.path.join("ai", "dataset", "lab7.csv"),
                    help="输出 CSV（相对实验7 根目录）")
    ap.add_argument("--board", type=int, choices=[1, 2], help="只导某块板")
    ap.add_argument("--limit", type=int, default=0, help="每板最多导多少条（0=全部）")
    ap.add_argument("--keep-alm", action="store_true", help="保留 ALM（超温报警）行")
    ap.add_argument("--min-per-board", type=int, default=0,
                    help="要求每板至少多少条，不足则退出码 1（题2 要求 ≥100）")
    args = ap.parse_args()

    conf = load_db_conf()
    if conf is None:
        print("✘ 没找到数据库配置。请先：\n"
              "    copy pc\\db_local.example.py pc\\db_local.py   → 填 DB_PASS\n"
              "  或设置环境变量 STM32_DB_USER / STM32_DB_PASS / STM32_DB_NAME")
        return 2
    try:
        import pymysql
    except ImportError:
        print("✘ 没装 pymysql：pip install pymysql")
        return 2

    sql = ("SELECT board,seq,up_ms,temp,humi,light,eco2,tvoc,status,"
           "sample_time,recv_time,ai_cls,ai_conf FROM lab7_data ")
    cond = []
    if not args.keep_alm:
        cond.append("status='OK'")
    if args.board:
        cond.append("board=%d" % args.board)
    if cond:
        sql += "WHERE " + " AND ".join(cond) + " "
    sql += "ORDER BY board, id"

    try:
        conn = pymysql.connect(charset="utf8mb4", autocommit=True, **conf)
    except Exception as e:
        print("✘ 连不上 MySQL：%s\n  （检查 pc/db_local.py 里的口令；"
              "表不存在就先跑 mysql -u root -p < pc/init_db.sql）" % e)
        return 2

    with conn.cursor() as cur:
        cur.execute(sql)
        rows = cur.fetchall()
        # ★ 顺手统计 ALM 行：采集时若没把报警阈值调高，室温一接近 28℃ 就会被判 ALM，
        #   而 ALM 行**默认被剔除** → 数据白采（真踩过：板1 采了 161 条，其中 119 条 ALM）。
        alm = {}
        if not args.keep_alm:
            cur.execute("SELECT board, COUNT(*) FROM lab7_data "
                        "WHERE status='ALM' GROUP BY board")
            for b, n in cur.fetchall():
                alm[b] = int(n)
    conn.close()

    # 每板限流（按 id 顺序取"最早的 N 条"，保持时间连续，方便画时间序列）
    if args.limit > 0:
        keep, cnt = [], {}
        for r in rows:
            b = r[0]
            cnt[b] = cnt.get(b, 0) + 1
            if cnt[b] <= args.limit:
                keep.append(r)
        rows = keep

    out_path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(COLS)
        w.writerows(rows)

    # ---- 统计 ----
    per = {}
    for r in rows:
        per[r[0]] = per.get(r[0], 0) + 1

    print("=" * 68)
    print("  导出 %s" % os.path.relpath(out_path, ROOT))
    print("=" * 68)
    for b in (1, 2):
        n = per.get(b, 0)
        flag = ""
        if args.min_per_board and n < args.min_per_board:
            flag = "  ← 不足 %d 条" % args.min_per_board
        extra = ""
        if alm.get(b, 0) > 0:
            extra = "   （另有 %d 条 ALM 被剔除）" % alm[b]
        print("  板%-2d %4d 条%s%s" % (b, n, extra, flag))
    print("  合计 %4d 条" % len(rows))
    if not args.keep_alm:
        print("  （已剔除 status=ALM 的行；要保留加 --keep-alm）")

    if any(alm.get(b, 0) >= 20 for b in (1, 2)):
        print("\n★ 有大量 ALM 行被剔除 —— 先确认原因，别急着采下一轮：")
        print("  · 多半是**报警阈值太低**（默认 28℃，室温就 27~28 时一碰就报）；")
        print("    采数据前在**板子串口敲 `a 35` + 回车**把阈值调到 35℃，再采一轮。")
        print("  · 也可能是真有人/热源靠近传感器（那几行本来就不该当训练数据，剔掉是对的）。")

    if args.min_per_board and any(per.get(b, 0) < args.min_per_board for b in (1, 2)):
        print("\n✘ 数据量不够（题2 要求两板各 ≥%d 条）—— 让板子多采一会儿再来。"
              % args.min_per_board)
        return 1
    print("\n✔ 可以进入下一步：\n"
          "    python tools/check_separability.py %s" % args.out.replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
