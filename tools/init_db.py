#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一键建库建表：把 pc/init_db.sql 应用到 MySQL（不需要装 mysql 命令行）。

为什么不用 `mysql -u root -p < pc/init_db.sql`：很多人机器上根本没把 mysql
客户端加进 PATH（本机就是这样）。这里用 pymysql 直接执行，口令仍然只放在
`pc/db_local.py`（已 gitignore，不会提交）。

用法：
    python tools/init_db.py            # 建 lab7_data（幂等：已存在就跳过，不清数据）
    python tools/init_db.py --check    # 只检查连接 + 表是否存在/多少行

执行前请先准备好口令：
    copy pc\\db_local.example.py pc\\db_local.py   → 把 DB_PASS 改成你的 MySQL 密码
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_conf():
    pc = os.path.join(ROOT, "pc")
    if pc not in sys.path:
        sys.path.insert(0, pc)
    try:
        import db_local as d
        return dict(host=d.DB_HOST, port=int(d.DB_PORT), user=d.DB_USER,
                    password=d.DB_PASS, database=None), d.DB_NAME
    except Exception as e:
        print("✘ 读不到 pc/db_local.py（%s）\n"
              "  先：copy pc\\db_local.example.py pc\\db_local.py，再改 DB_PASS" % e)
        return None, None


def split_sql(text):
    """去掉 `--` 注释行，按 ';' 拆语句。"""
    out, buf = [], []
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("--"):
            continue
        buf.append(line)
        if s.endswith(";"):
            out.append("\n".join(buf).strip().rstrip(";"))
            buf = []
    if buf:
        out.append("\n".join(buf).strip())
    return [s for s in out if s]


def main():
    ap = argparse.ArgumentParser(description="把 pc/init_db.sql 应用到 MySQL")
    ap.add_argument("--check", action="store_true", help="只做连接与表检查")
    args = ap.parse_args()

    conf, dbname = load_conf()
    if conf is None:
        return 2
    try:
        import pymysql
    except ImportError:
        print("✘ 没装 pymysql：pip install pymysql")
        return 2

    # 先连（不指定库，因为库可能还不存在）
    try:
        conn = pymysql.connect(charset="utf8mb4", autocommit=True, **conf)
    except Exception as e:
        print("✘ 连不上 MySQL：%s" % e)
        return 2
    print("✔ 已连上 MySQL：%s@%s:%d" % (conf["user"], conf["host"], conf["port"]))

    if not args.check:
        sql_path = os.path.join(ROOT, "pc", "init_db.sql")
        stmts = split_sql(open(sql_path, encoding="utf-8").read())
        print("  执行 %s（%d 条语句，幂等）" % (os.path.relpath(sql_path, ROOT), len(stmts)))
        with conn.cursor() as cur:
            for s in stmts:
                head = re.sub(r"\s+", " ", s)[:60]
                try:
                    cur.execute(s)
                    if s.upper().startswith(("SELECT", "SHOW")):
                        rows = cur.fetchall()
                        for r in rows[:20]:
                            print("    %s → %s" % (head, r))
                except Exception as e:
                    print("    ✘ %s … → %s" % (head, e))
                    return 1

    # 检查
    print("\n【检查】")
    with conn.cursor() as cur:
        cur.execute("SHOW DATABASES LIKE %s", (dbname,))
        if not cur.fetchone():
            print("  ✘ 库 %s 不存在 —— 先不带 --check 跑一次" % dbname)
            return 1
        cur.execute("USE `%s`" % dbname)
        cur.execute("SHOW TABLES LIKE 'lab7_data'")
        if not cur.fetchone():
            print("  ✘ 表 lab7_data 不存在 —— 先不带 --check 跑一次")
            return 1
        cur.execute("SELECT board, COUNT(*) FROM lab7_data GROUP BY board")
        rows = cur.fetchall()
        cur.execute("SELECT COUNT(*) FROM lab7_data")
        total = cur.fetchone()[0]
        print("  ✔ 库 %s / 表 lab7_data 就绪，共 %d 行" % (dbname, total))
        for b, n in rows:
            print("     板%s：%d 条%s" % (b, n, "" if n >= 100 else "（不到 100，题2 要求每板 ≥100）"))
        if total == 0:
            print("     表是空的 —— 正常：烧好板子跑上位机，数据会自动写进来。")
    conn.close()
    print("\n下一步：python pc/upper_computer.py   （看第一行是否显示已连上 MySQL）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
