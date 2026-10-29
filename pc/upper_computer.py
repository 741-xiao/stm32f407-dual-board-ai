8# -*- coding: utf-8 -*-
"""
实验七 · PC 上位机（单文件，零依赖，只用标准库）

用途：同时接收**两块板**发来的 TCP 帧，动态打表 + **五种风格**图形
     （折线 / 柱状 / 面积 / 饼图 / 散点，分两页）。
     板1/板2 -> PC:8080   两条连接各自一线程；
     **身份靠 H/D 帧里的 board 字段识别**（协议 v7），
     不依赖谁先连进来、也不依赖源 IP。

帧格式（协议 v7，见 方案设计.md §5）：
    H,<姓名>,<学号>,<board>                                          握手（连接就绪时一次）
    D,<board>,<up_ms>,<seq>,<温度>.<一位>,<湿度>.<一位>,<光>,<CO2>,<TVOC>,<OK/ALM>[,<ai_cls>,<ai_conf>]
                                                                     数据（1Hz；★ AI 两字段可缺省）
    S,<board>,START|STOP                                             采集开关状态（事件驱动）
    ai_cls: 0=未推理 1=判为板1 2=判为板2；ai_conf: 0..100（%）

设计要点：
    * 一连接一线程：ESP 掉电重启不发 FIN，旧连接会"假活着"，这样新连接照样能被接受。
    * 时间标定**按板分开**：每板各自用"最小偏移"标定 offset = PC时间 - up_ms/1000；
      ★ 收到 H 帧要重置该板标定（板子重启后 up_ms 从 0 重算）。
    * H 帧姓名是 GB18030，用 decode("gbk") 还原。
    * MySQL 入库可选：口令放 pc/db_local.py（.gitignore 排除）；没有就只收/只显示。
      默认**保留**库里已有数据（要清空：设环境变量 STM32_DB_CLEAR=1 再启动）。
    * 在线判据 = "收到该板任何帧都算在线"（别绑 S 帧，它是事件驱动的）。

用法：
    1) 电脑开移动热点；
    2) python pc/upper_computer.py      （网页：http://127.0.0.1:8000）
    3) 两块板上电（**上电都不采**）；按**任意一块板**的 KEY0 → 两板同时开始采集。
    退出：Ctrl+C。
"""

import json
import os
import socket
import sys
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

try:
    sys.stdout.reconfigure(errors="replace")     # 中文打不出来时不至于崩
except Exception:
    pass

HOST = "0.0.0.0"
# ★ 端口可用环境变量覆盖 —— 方便"离线冒烟"：用另一套端口起一份，不动正在跑的正式服务。
#   （原来要"改端口副本"，现在：set STM32_TCP_PORT=18080 & set STM32_WEB_PORT=18000）
PORT = int(os.environ.get("STM32_TCP_PORT", "8080"))
WEB_PORT = int(os.environ.get("STM32_WEB_PORT", "8000"))

# 两块板的显示信息（板号 = 协议里的 board 字段）
# ★ 实验七只有两块板（实验六是三块），板3 已删除。配色沿用实验五/六。
BOARDS = {
    1: {"name": "板1", "color": "#378ADD"},
    2: {"name": "板2", "color": "#EF9F27"},
}
ALL_BOARDS = (1, 2)

# ---- MySQL 入库（可选）----------------------------------------------------
# 口令放本地 pc/db_local.py（已被 .gitignore 排除，不入库）：
#     DB_HOST="127.0.0.1"; DB_PORT=3306; DB_USER="root"
#     DB_PASS="你的密码";   DB_NAME="stm32_lab"
# 没有这个文件 / 没装 pymysql 时，自动跳过入库，接收和显示照常。
# ★ PyMySQL 的连接【不是线程安全的】：收数据线程(INSERT) 和 网页线程(SELECT/UPDATE)
#   会同时用它，交叉使用会把协议搅乱。所以所有 DB 操作都要过这把锁串行执行。
_dlock = threading.Lock()
_db_err_shown = False               # 出错只报一次，别刷屏

_db = None
try:
    import pymysql
    try:
        from db_local import DB_HOST, DB_PORT, DB_USER, DB_PASS, DB_NAME
    except ImportError:
        DB_HOST = os.environ.get("STM32_DB_HOST", "127.0.0.1")
        DB_PORT = int(os.environ.get("STM32_DB_PORT", "3306"))
        DB_USER = os.environ.get("STM32_DB_USER", "root")
        DB_PASS = os.environ.get("STM32_DB_PASS", "")
        DB_NAME = os.environ.get("STM32_DB_NAME", "stm32_lab")
    _db = pymysql.connect(host=DB_HOST, port=DB_PORT, user=DB_USER,
                          password=DB_PASS, database=DB_NAME,
                          charset="utf8mb4", autocommit=True)
    print("[DB] 已连上 MySQL：%s@%s/%s（数据会写进 lab7_data）" % (DB_USER, DB_HOST, DB_NAME))
except Exception as _e:
    _db = None
    print("[DB] 未启用 MySQL 入库（%s）—— 只接收/显示，不入库" % _e)


def _db_reconnect():
    """连接坏了就重连一次（被服务器断掉 / 协议错乱后）。"""
    global _db
    try:
        if _db is not None:
            _db.close()
    except Exception:
        pass
    _db = None
    try:
        _db = pymysql.connect(host=DB_HOST, port=DB_PORT, user=DB_USER,
                              password=DB_PASS, database=DB_NAME,
                              charset="utf8mb4", autocommit=True)
        pr("[DB] 已重连 MySQL")
        return True
    except Exception as e:
        pr("[DB] 重连失败：%s" % e)
        return False


def _db_call(job):
    """串行执行一次数据库操作（拿锁）；出错就重连再试一次。返回 (ok, result)。"""
    global _db_err_shown
    if _db is None:
        return False, None
    with _dlock:
        for attempt in (0, 1):
            try:
                r = job(_db)
                _db_err_shown = False
                return True, r
            except Exception as e:
                if attempt == 0:
                    if not _db_err_shown:
                        pr("[DB] 操作出错(%s)，重连后重试…" % e)
                        _db_err_shown = True
                    if not _db_reconnect():
                        return False, None
                else:
                    if not _db_err_shown:
                        pr("[DB] 操作仍失败：%s" % e)
                        _db_err_shown = True
    return False, None


# ---- 每块板各自的时间标定：offset = PC时间 - up_ms/1000（用"最小偏移"标定）----
_t_offset = {1: None, 2: None}

# ---- 握手信息（H 帧）----
stu = {1: ("?", "?"), 2: ("?", "?")}

# ---- 统计 ----
rx_count = 0

# ============================================================================
#  采集启停状态（协议 v7 的 S 帧）—— 题1「web 页面显示采集开始或停止」
#      S,<board>,START|STOP
#      · 板1：按 KEY0(按键1) 发 START、KEY1(按键2) 发 STOP；
#      · 板2：上电即采，握手后补发一条 START；
#      · 握手成功后固件会再补发一次"当前状态"，所以网页一打开就能看到。
#  超时兜底：COLLECT_OFFLINE_S 秒没收到该板**任何帧** → 判「未连接」。
#     ★ 判据必须是"任何帧"，不能只看 S 帧：S 帧是**事件驱动**的（状态变了才发），
#       板2 上电即采 → 握手后只发一条 S,2,START，之后再不发 S。
#       若拿"S 帧多久没来"当在线判据，板2 十秒后就会被误判成"未连接"
#       （实测踩到：数据一直在刷，状态条却显示未连接）。
#       D 帧是 1Hz 的，用"任何帧"判在线才准。
# ============================================================================
COLLECT_OFFLINE_S = 10.0
_collect = {1: {"state": None, "ai_cls": None, "ai_conf": None},
            2: {"state": None, "ai_cls": None, "ai_conf": None}}
_seen = {1: 0.0, 2: 0.0}            # 每板最后一次收到帧的时刻（任何帧）
_collect_lock = threading.Lock()


def on_status(board, text):
    """处理 S 帧。"""
    st = text.strip().upper()
    if st not in ("START", "STOP"):
        pr("[状态] %s：无法识别的状态 %r" % (BOARDS[board]["name"], text))
        return
    with _collect_lock:
        _collect[board]["state"] = st
        _seen[board] = time.time()
    pr("[状态] %s：采集%s" % (BOARDS[board]["name"], "开始 ▶" if st == "START" else "停止 ■"))


def collect_status():
    """给网页 /api/status 用：每板采集状态 + 在线判定 + 主控（板1）总状态。"""
    now = time.time()
    out = {"boards": {}, "master_state": "UNKNOWN"}
    for b in ALL_BOARDS:
        with _collect_lock:
            st = _collect[b]["state"]
            ai_cls = _collect[b]["ai_cls"]
            ai_conf = _collect[b]["ai_conf"]
            last = _seen[b]
        age = (now - last) if last else None
        online = (age is not None) and (age < COLLECT_OFFLINE_S)
        out["boards"][str(b)] = {
            "name": BOARDS[b]["name"], "color": BOARDS[b]["color"],
            "state": (st if online else None),      # 掉线就不敢再报状态
            "online": bool(online),
            "age_s": (None if age is None else round(age, 1)),
            "ai_cls": (ai_cls if online else None),
            "ai_conf": (ai_conf if online else None),
        }
    b1 = out["boards"]["1"]
    if b1["online"]:
        out["master_state"] = b1["state"] or "UNKNOWN"
    elif b1["age_s"] is None:
        out["master_state"] = "UNKNOWN"        # 从没收到过任何帧
    else:
        out["master_state"] = "OFFLINE"
    out["stu"] = {str(b): {"name": stu[b][0], "sid": stu[b][1]} for b in ALL_BOARDS}
    return out

print_lock = threading.Lock()


def pr(s):
    with print_lock:
        print(s, flush=True)


def print_header():
    pr("=" * 104)
    pr("  实验七 · 上位机   监听 %s:%d（板1/板2 各一条连接）" % (HOST, PORT))
    pr("  页面① 记录列表（分页） http://127.0.0.1:%d/      页面② 图形 http://127.0.0.1:%d/chart"
       % (WEB_PORT, WEB_PORT))
    pr("=" * 104)
    pr("%-5s %-8s %-10s %-8s %-8s %-7s %-8s %-8s %-5s %-5s %-6s %s" %
       ("板", "序号", "up_ms", "温度C", "湿度%", "光敏", "CO2ppm", "TVOCppb",
        "状态", "AI板", "AI%", "采集时间"))
    pr("-" * 104)


def _stamp(board, up_ms):
    """把该板的 up_ms 换算成 PC 时钟的**采集时刻**，返回 (sample_dt, recv_dt)。
       用"最小偏移"标定（取延迟最低的那次）：链路中断/排队会让某帧晚到很多，
       只认最小值，sample_time 才不会被带偏、始终不晚于 recv_time。"""
    now = time.time()
    est = now - (float(up_ms) / 1000.0)
    cur = _t_offset.get(board)
    if (cur is None) or (est < cur):
        _t_offset[board] = est
    sample_ts = float(up_ms) / 1000.0 + _t_offset[board]
    if sample_ts > now:
        sample_ts = now          # 保底：采集时刻绝不晚于收到时刻
    return datetime.fromtimestamp(sample_ts), datetime.fromtimestamp(now)


def db_insert(board, seq, temp, humi, light, eco2, tvoc, status, up_ms,
              ai_cls=None, ai_conf=None):
    """把一帧数据写进 stm32_lab.lab7_data（需 DB 可用；否则直接返回 None）。
       ai_cls/ai_conf：协议 v7 追加的板端神经网络判定（可为 None）。"""
    if _db is None:
        return None
    sample_dt, recv_dt = _stamp(board, up_ms)

    def job(conn):
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO lab7_data "
                "(board, seq, temp, humi, light, eco2, tvoc, status, up_ms, "
                " sample_time, recv_time, ai_cls, ai_conf) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
                (int(board), int(seq), float(temp), float(humi), int(light),
                 int(eco2), int(tvoc),
                 str(status).strip(), int(up_ms), sample_dt, recv_dt,
                 ai_cls, ai_conf))
            return cur.lastrowid

    ok, rid = _db_call(job)
    return rid if ok else None


def on_handshake(board, name, sid):
    stu[board] = (name, sid)
    # ★ 收到 H 帧 = 该板刚（重）连上。板子重启后 up_ms 会从 0 重新计，
    #   而 _t_offset 用的是"历史最小偏移"——不重置的话，新的一轮 up_ms 会被
    #   套用旧偏移，sample_time 直接回跳到很久以前（采集时间看着就不对了）。
    #   置 None → 用这一轮的第一帧重新标定；后续帧继续走"取最小偏移"自动收敛。
    _t_offset[board] = None
    with _collect_lock:
        _seen[board] = time.time()          # 握手也算"这块板还在"
    pr("[握手] %s：姓名=%s  学号=%s（时间标定已重置）"
       % (BOARDS[board]["name"], name, sid))


def on_data(board, f):
    """f = ["D", board, up_ms, seq, temp, humi, light, eco2, tvoc, status]
       协议 v6 = 10 字段；**v7 追加两个可选字段** ai_cls / ai_conf：
         ai_cls 0=未推理 1=判为板1 2=判为板2；ai_conf 0..100。
       两个字段都可缺省（阶段5 之前固件不发），所以按长度可选解析。"""
    global rx_count
    if len(f) < 10:
        return
    up_ms, seq, temp, humi, light = f[2], f[3], f[4], f[5], f[6]
    eco2, tvoc, status = f[7], f[8], f[9]
    ai_cls = None
    ai_conf = None
    if len(f) > 10 and f[10].strip().isdigit() and f[10].strip() != "0":
        ai_cls = int(f[10])
    if len(f) > 11 and f[11].strip().isdigit():
        ai_conf = int(f[11])
    rx_count += 1
    # ★ 收到任何一帧都算"这块板还在"（在线判据用它，见 collect_status）
    with _collect_lock:
        _seen[board] = time.time()
        if ai_cls:
            _collect[board]["ai_cls"] = ai_cls
            _collect[board]["ai_conf"] = ai_conf
    sample_dt, recv_dt = _stamp(board, up_ms)
    pr("%-5s %-8s %-10s %-8s %-8s %-7s %-8s %-8s %-5s %-5s %-6s %s" %
       (BOARDS[board]["name"], seq, up_ms, temp, humi, light, eco2, tvoc, status,
        ("板%d" % ai_cls) if ai_cls else "-",
        ("%d%%" % ai_conf) if ai_conf is not None else "-",
        sample_dt.strftime("%Y-%m-%d %H:%M:%S")))

    rid = db_insert(board, seq, temp, humi, light, eco2, tvoc, status, up_ms,
                    ai_cls, ai_conf)
    remember({
        "id": rid if rid is not None else -rx_count,   # 没数据库时用负数占位
        "board": int(board), "seq": int(seq), "temp": float(temp), "humi": float(humi),
        "light": int(light), "eco2": int(eco2), "tvoc": int(tvoc),
        "status": str(status).strip(), "up_ms": int(up_ms),
        "ai_cls": ai_cls, "ai_conf": ai_conf,
        "sample_time": sample_dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
        "recv_time": recv_dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
    })


def on_control(board, text):
    pr("[控制] %s：%s" % (BOARDS[board]["name"], text.strip()))


def _board_of(fields):
    """从帧的第 2 个字段取板号（1/2/3）；非法返回 None。"""
    if len(fields) >= 2 and fields[1] in ("1", "2", "3"):
        return int(fields[1])
    return None


def on_line(text):
    """text 已经 strip 过，且是 GBK 解码后的字符串。"""
    if text.startswith("H,"):
        # H,<姓名>,<学号>,<board>  —— ★ 板号在**最后一个**字段！
        # （原来错用 _board_of(p) 取 p[1]，那取到的是姓名，永远不是 "1"/"2"/"3"，
        #   结果 4 字段的 H 帧被整条丢掉 → 控制台永远看不到 "[握手]" 那行。）
        p = text.split(",")
        if len(p) >= 4:
            b = int(p[3]) if p[3] in ("1", "2", "3") else 1
            on_handshake(b, p[1], p[2])
        elif len(p) >= 3:
            on_handshake(1, p[1], p[2])          # 兼容旧格式（无板号 → 板1）
    elif text.startswith("D,"):
        p = text.split(",")
        b = _board_of(p)                          # D,<board>,... —— 板号就在 p[1]
        if b:
            on_data(b, p)
    elif text.startswith("S,"):
        # S,<board>,START|STOP —— 板号在 p[1]（与 D/C 帧一致）
        p = text.split(",")
        b = _board_of(p)
        if b and len(p) >= 3:
            on_status(b, p[2])
    elif text.startswith("C,"):
        p = text.split(",")
        b = _board_of(p)
        if b:
            on_control(b, ",".join(p[1:]))
    else:
        pr("[?] %s" % text)


# ============================================================================
#  网页（标准库 http.server，零依赖）—— 列表（带"板"列）+ 三张曲线
#  浏览器打开 http://127.0.0.1:8000
# ============================================================================
RECENT_MAX = 1000
_recent = []                        # 内存里最近若干条（没数据库时页面也能看）
_recent_lock = threading.Lock()


def remember(rec):
    with _recent_lock:
        _recent.append(rec)
        if len(_recent) > RECENT_MAX:
            del _recent[0:len(_recent) - RECENT_MAX]


def fetch_rows(limit=400):
    if _db is not None:
        def job(conn):
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT id,board,seq,temp,humi,light,eco2,tvoc,status,up_ms,"
                    "sample_time,recv_time,ai_cls,ai_conf "
                    "FROM lab7_data ORDER BY id DESC LIMIT %s", (limit,))
                cols = [c[0] for c in cur.description]
                out = []
                for row in cur.fetchall():
                    d = dict(zip(cols, row))
                    d["temp"] = float(d["temp"])
                    d["humi"] = float(d["humi"])
                    d["board"] = int(d["board"])
                    d["sample_time"] = str(d["sample_time"])
                    d["recv_time"] = str(d["recv_time"])
                    d["ai_cls"] = (int(d["ai_cls"]) if d.get("ai_cls") is not None else None)
                    d["ai_conf"] = (int(d["ai_conf"]) if d.get("ai_conf") is not None else None)
                    out.append(d)
                return out
        ok, res = _db_call(job)
        if ok:
            return res
    with _recent_lock:
        return list(reversed(_recent[-limit:]))


def clear_all():
    """清空 lab7_data 并把自增归零。启动时自动调用；网页上也有「清空」按钮。"""
    n = 0
    if _db is not None:
        def job(conn):
            with conn.cursor() as cur:
                cur.execute("SELECT COUNT(*) FROM lab7_data")
                k = cur.fetchone()[0]
                cur.execute("DELETE FROM lab7_data")
                cur.execute("ALTER TABLE lab7_data AUTO_INCREMENT = 1")
                return k
        ok, res = _db_call(job)
        if ok:
            n = res
    with _recent_lock:
        _recent[:] = []
    return n


def db_count():
    """库里现有多少行（DB 不可用时退回内存里的条数）。启动时打印用。"""
    if _db is not None:
        def job(conn):
            with conn.cursor() as cur:
                cur.execute("SELECT COUNT(*) FROM lab7_data")
                return int(cur.fetchone()[0])
        ok, n = _db_call(job)
        if ok:
            return n
    with _recent_lock:
        return len(_recent)


def update_row(rid, temp, humi, light, eco2, tvoc, status):
    if _db is not None:
        def job(conn):
            with conn.cursor() as cur:
                cur.execute("UPDATE lab7_data SET temp=%s, humi=%s, light=%s, eco2=%s, tvoc=%s, status=%s WHERE id=%s",
                            (temp, humi, light, eco2, tvoc, status, rid))
        _db_call(job)
    with _recent_lock:
        for r in _recent:
            if r.get("id") == rid:
                r.update({"temp": temp, "humi": humi, "light": light,
                          "eco2": eco2, "tvoc": tvoc, "status": status})


def delete_row(rid):
    if _db is not None:
        def job(conn):
            with conn.cursor() as cur:
                cur.execute("DELETE FROM lab7_data WHERE id=%s", (rid,))
        _db_call(job)
    with _recent_lock:
        _recent[:] = [r for r in _recent if r.get("id") != rid]


_TPL = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>实验七 · 两板采集数据 · __PAGE_NAME__</title>
<style>
 body{font-family:system-ui,"Microsoft YaHei",sans-serif;margin:16px;background:#f7f8fa;color:#222}
 h1{font-size:18px;margin:0 0 6px}
 h1 .sign{font-size:13px;font-weight:400;color:#8a9099;margin-left:12px}
 #bar{font-size:13px;color:#666;margin-bottom:8px}
 table{border-collapse:collapse;width:100%;background:#fff;font-size:14px}
 th,td{border:1px solid #e3e6ea;padding:6px 8px;text-align:center;white-space:nowrap}
 th{background:#eef2f6;cursor:pointer;user-select:none}
 th .arw{color:#9aa4b0;font-size:11px}
 tr:nth-child(even) td{background:#fafbfc}
 .ok{color:#0a7d32;font-weight:600}
 .alm{color:#c0392b;font-weight:600}
 .b1{background:#378ADD;color:#fff;border-radius:3px;padding:1px 6px;font-weight:600}
 .b2{background:#EF9F27;color:#fff;border-radius:3px;padding:1px 6px;font-weight:600}
 .b3{background:#2E9E5B;color:#fff;border-radius:3px;padding:1px 6px;font-weight:600}
 h3.sec{font-size:13px;color:#444;margin:14px 0 6px;padding-left:8px;border-left:3px solid #378ADD}
 .grid{display:flex;flex-wrap:wrap;gap:10px}
 .grid .wrap{margin-bottom:0}
 button{padding:3px 9px;border:1px solid #c8cdd4;background:#fff;border-radius:4px;cursor:pointer}
 button:hover{background:#f0f3f7}
 input.ed{width:66px;padding:2px 4px;text-align:center}
 .err{color:#c0392b}
 #tools{margin-bottom:10px;font-size:14px}
 /* ---- 顶部导航（题1：分 2 页，可互相切换）---- */
 #nav{font-size:14px;margin-bottom:10px}
 #nav a{display:inline-block;padding:6px 16px;margin-right:6px;border:1px solid #c8cdd4;
        border-radius:6px;text-decoration:none;color:#333;background:#fff}
 #nav a.on{background:#378ADD;border-color:#378ADD;color:#fff;font-weight:600}
 /* ---- 采集状态条（题1：网页显示采集开始/停止）---- */
 #sbar{display:flex;align-items:center;gap:14px;flex-wrap:wrap;background:#fff;
       border:1px solid #e3e6ea;border-radius:6px;padding:8px 12px;margin-bottom:10px;font-size:14px}
 #sbar .lbl{color:#666}
 #sbar .pill{padding:3px 12px;border-radius:12px;font-weight:700;color:#fff;background:#9aa4b0}
 #sbar .pill.on{background:#0a7d32}
 #sbar .pill.off{background:#8a9099}
 #sbar .pill.unknown{background:#c0392b}
 #sbar .bd{font-size:13px;color:#555}
 #sbar .who{margin-left:auto;font-size:13px;color:#888}
 #tools select,#tools input{padding:2px 6px;border:1px solid #c8cdd4;border-radius:4px}
 .wrap{background:#fff;border:1px solid #e3e6ea;border-radius:6px;padding:6px;margin-bottom:10px}
 .wrap h2{font-size:13px;color:#555;margin:2px 0 4px 6px;font-weight:600}
 #pager{margin-top:10px;font-size:14px;display:flex;align-items:center;gap:8px;flex-wrap:wrap}
 #pager button:disabled{opacity:.45;cursor:not-allowed}
 #pager .pinfo{color:#666}
 #pager select{padding:2px 6px;border:1px solid #c8cdd4;border-radius:4px}
 #pager .jump{width:56px;padding:2px 6px;border:1px solid #c8cdd4;border-radius:4px;text-align:center}
</style>
</head>
<body>
<h1>实验七 · 两块 STM32F407 采集数据<span class="sign" id="sign">（等待板子握手…）</span></h1>
<div id="nav">
  <a id="nav_list" href="/">① 记录列表（分页）</a>
  <a id="nav_chart" href="/chart">② 图形（多风格）</a>
</div>

<!-- 采集状态条：由板端 S 帧驱动（板1 KEY0=开始 / KEY1=停止） -->
<div id="sbar">
  <span class="lbl">板1 采集状态</span>
  <span class="pill unknown" id="pill_all">未知</span>
  <span class="bd" id="bd1">板1：—</span>
  <span class="bd" id="bd2">板2：—</span>
  <span class="who" id="who">—</span>
</div>

<div id="bar">加载中…</div>
<!--LIST_START-->
<div id="tools">
  板 <select id="f_board"><option value="">全部</option><option value="1">板1</option><option value="2">板2</option></select>
  &nbsp;状态 <select id="f_status"><option value="">全部</option><option value="OK">OK</option><option value="ALM">ALM</option></select>
  &nbsp;序号 <input id="f_seq1" style="width:64px" placeholder="起"> ~ <input id="f_seq2" style="width:64px" placeholder="止">
  &nbsp;<button onclick="load()">应用</button>
  &nbsp;<button onclick="exportCSV()">导出 CSV</button>
   &nbsp;<button onclick="clearAll()">清空数据</button>
  &nbsp;<a href="/chart"><button type="button">去图形页 ›</button></a>
</div>
<table>
  <thead><tr>
    <th onclick="sortBy('id')">ID<span class="arw" id="a_id"></span></th>
    <th onclick="sortBy('board')">板<span class="arw" id="a_board"></span></th>
    <th onclick="sortBy('seq')">序号<span class="arw" id="a_seq"></span></th>
    <th onclick="sortBy('temp')">温度℃<span class="arw" id="a_temp"></span></th>
    <th onclick="sortBy('humi')">湿度%<span class="arw" id="a_humi"></span></th>
    <th onclick="sortBy('light')">光敏<span class="arw" id="a_light"></span></th>
    <th onclick="sortBy('eco2')">CO₂ ppm<span class="arw" id="a_eco2"></span></th>
    <th onclick="sortBy('tvoc')">TVOC ppb<span class="arw" id="a_tvoc"></span></th>
    <th>状态</th><th>采集时间</th><th>收到时间</th>
    <th>AI 判定</th><th>操作</th>
  </tr></thead>
  <tbody id="tb"></tbody>
</table>
<div id="pager">
  每页 <select id="p_size" onchange="onPageSize()">
    <option value="20">20</option><option value="50" selected>50</option>
    <option value="100">100</option><option value="200">200</option><option value="500">500</option>
  </select> 条
  &nbsp;<button id="p_first" onclick="gotoPage(0)">« 首页</button>
  &nbsp;<button id="p_prev" onclick="gotoPage(page-1)">‹ 上一页</button>
  &nbsp;<span class="pinfo" id="p_info">-</span>
  &nbsp;<button id="p_next" onclick="gotoPage(page+1)">下一页 ›</button>
  &nbsp;<button id="p_last" onclick="gotoPage(pageTotal-1)">末页 »</button>
  &nbsp;跳至 <input class="jump" id="p_jump" type="number" min="1" onkeydown="if(event.key==='Enter')jumpTo()"> 页
  &nbsp;<button onclick="jumpTo()">Go</button>
</div>
<!--LIST_END-->

<!--CHART_START-->
<div id="charts">
<!--CHARTS-->
</div>
<!--CHART_END-->
<script>
/* KIND: 'list' = 记录列表页，'chart' = 图形页（同一份 JS，两页共用）*/
const KIND = '__KIND__';
const ALLB = [1, 2];                    /* 实验七两块板（原来写死 [1,2,3]）*/
let editing = null, rowsAll = [], sortKey = 'id', sortAsc = false, showChart = true;
let page = 0, pageSize = 50, pageTotal = 1;   /* ★ 分页状态：当前页/每页条数/总页数 */
const BC = {1: '#378ADD', 2: '#EF9F27'};
const CONV = { light: function(v){ return v / 4095 * 100; } };
// 每种数据一种风格、互不重复（与 Python 侧 _CHART_PARAMS 一一对应）
const CHARTS = [
  {key:'temp',  style:'line'},
  {key:'humi',  style:'bar'},
  {key:'light', style:'area'},
  {key:'eco2',  style:'pie'},
  {key:'tvoc',  style:'scatter'}
];
function valOf(d, key){ const c = CONV[key]; return c ? c(d[key]) : d[key]; }
function seriesOf(b, key){
  return rowsAll.filter(function(d){ return d.board === b; })
                .sort(function(x, y){ return x.up_ms - y.up_ms; })
                .slice(-150)
                .map(function(d){ return { v: valOf(d, key) }; });
}

function applyView(rows){
  let r = rows.slice();
  /* ★ 筛选控件只存在于**列表页**；图形页要共用这个函数，所以每个元素都得判空 ——
     否则图形页上 getElementById 返回 null，取 .value 抛异常，整个 load() 落进 catch，
     图形页会一直显示"取数据失败"。（这一条是拆两页时最容易踩的坑。）*/
  const eb = document.getElementById('f_board');
  const es = document.getElementById('f_status');
  const e1 = document.getElementById('f_seq1');
  const e2 = document.getElementById('f_seq2');
  const bd = eb ? eb.value : '';
  if (bd) r = r.filter(function(d){ return String(d.board) === bd; });
  const st = es ? es.value : '';
  if (st) r = r.filter(function(d){ return d.status === st; });
  const a = e1 ? parseInt(e1.value, 10) : NaN;
  const b = e2 ? parseInt(e2.value, 10) : NaN;
  if (!isNaN(a)) r = r.filter(function(d){ return d.seq >= a; });
  if (!isNaN(b)) r = r.filter(function(d){ return d.seq <= b; });
  const s = sortAsc ? 1 : -1;
  r.sort(function(x, y){ return (x[sortKey] > y[sortKey] ? 1 : x[sortKey] < y[sortKey] ? -1 : 0) * s; });
  return r;
}

function sortBy(k){
  if (sortKey === k) { sortAsc = !sortAsc; } else { sortKey = k; sortAsc = false; }
  ['id','board','seq','temp','humi','light','eco2','tvoc'].forEach(function(x){
    const e = document.getElementById('a_' + x);
    if (e) e.textContent = (x === sortKey) ? (sortAsc ? '▲' : '▼') : '';
  });
  load();
}

function exportCSV(){
  const rows = applyView(rowsAll);
  const head = ['id','board','seq','temp','humi','light','eco2','tvoc','status','up_ms','sample_time','recv_time','ai_cls','ai_conf'];
  const lines = [head.join(',')];
  for (const d of rows) lines.push(head.map(function(k){ return d[k]; }).join(','));
  const blob = new Blob(['﻿' + lines.join('\r\n')], {type:'text/csv;charset=utf-8'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'lab7_data_' + new Date().toISOString().slice(0,19).replace(/[:T]/g,'') + '.csv';
  a.click();
  setTimeout(function(){ URL.revokeObjectURL(a.href); }, 1000);
}

/* 图形不再"显示/隐藏"——已独立成第 2 页（/chart）*/

// ---- 五种风格：折线 / 柱状 / 面积 / 饼图 / 散点；每张图两条线（板1 / 板2）----
function chartRange(ser){
  let vmin = Infinity, vmax = -Infinity;
  for (const s of ser) for (const p of s){ if (p.v < vmin) vmin = p.v; if (p.v > vmax) vmax = p.v; }
  if (!isFinite(vmin)){ vmin = 0; vmax = 100; }
  if (vmax - vmin < 1e-6){ vmax = vmin + 1; }
  const pad = (vmax - vmin) * 0.1;
  return [vmin - pad, vmax + pad];
}

function chartFrame(g, W, H, vmin, vmax){
  const L = 46, R = W - 12, T = 24, B = H - 14;
  g.strokeStyle = '#eef2f6'; g.lineWidth = 1; g.beginPath();
  for (let i = 0; i <= 4; i++){ const y = B - (B - T) * i / 4; g.moveTo(L, y); g.lineTo(R, y); }
  g.stroke();
  g.fillStyle = '#9aa4b0'; g.font = '11px sans-serif';
  for (let i = 0; i <= 4; i++){
    const y = B - (B - T) * i / 4;
    g.fillText((vmin + (vmax - vmin) * i / 4).toFixed(1), 4, y + 4);
  }
  return {L: L, R: R, T: T, B: B};
}

function chartLegend(g){
  let lx = 46;
  for (const b of ALLB){
    g.fillStyle = BC[b]; g.fillRect(lx, 6, 9, 9);
    g.fillStyle = '#555'; g.font = '11px sans-serif';
    g.fillText('板' + b, lx + 12, 14);
    lx += 56;
  }
}

function emptyNote(g, F, msg){
  g.fillStyle = '#999'; g.font = '12px sans-serif';
  g.fillText(msg, F.L + 8, F.T + 18);
}

// 折线 / 面积（style = 'line' | 'area'）
function chartLine(cvId, key, style){
  const cv = document.getElementById(cvId);
  if (!cv || !showChart) return;
  const g = cv.getContext('2d'), W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  g.fillStyle = '#fff'; g.fillRect(0, 0, W, H);

  const ser = ALLB.map(function(b){ return seriesOf(b, key); });
  const rng = chartRange(ser), vmin = rng[0], vmax = rng[1];
  const F = chartFrame(g, W, H, vmin, vmax);
  chartLegend(g);

  let drew = 0;
  for (let bi = 0; bi < ALLB.length; bi++){
    const b = ALLB[bi], pts = ser[bi];
    if (pts.length < 2) continue;
    drew++;
    const X = function(i){ return F.L + (F.R - F.L) * i / (pts.length - 1); };
    const Y = function(v){ return F.B - (F.B - F.T) * (v - vmin) / (vmax - vmin); };
    if (style === 'area'){
      g.fillStyle = BC[b] + '33';
      g.beginPath();
      g.moveTo(X(0), F.B);
      for (let i = 0; i < pts.length; i++) g.lineTo(X(i), Y(pts[i].v));
      g.lineTo(X(pts.length - 1), F.B);
      g.closePath(); g.fill();
    }
    g.strokeStyle = BC[b]; g.lineWidth = 2; g.beginPath();
    for (let i = 0; i < pts.length; i++){
      if (i) g.lineTo(X(i), Y(pts[i].v)); else g.moveTo(X(i), Y(pts[i].v));
    }
    g.stroke();
  }
  if (drew === 0) emptyNote(g, F, '数据不足，攒够 2 个点再画…');
}

// 柱状：每板取最新一个点，三根柱对比
function chartBar(cvId, key){
  const cv = document.getElementById(cvId);
  if (!cv || !showChart) return;
  const g = cv.getContext('2d'), W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  g.fillStyle = '#fff'; g.fillRect(0, 0, W, H);

  const last = ALLB.map(function(b){
    const s = seriesOf(b, key);
    return s.length ? s[s.length - 1].v : null;
  });
  let vmax = 0;
  for (const v of last) if (v !== null && v > vmax) vmax = v;
  if (vmax <= 0) vmax = 1;

  const F = chartFrame(g, W, H, 0, vmax);
  chartLegend(g);

  const slot = (F.R - F.L) / ALLB.length, bw = slot * 0.5;
  let any = false;
  for (let bi = 0; bi < ALLB.length; bi++){
    if (last[bi] === null) continue;
    any = true;
    const x = F.L + slot * bi + (slot - bw) / 2;
    const y = F.B - (F.B - F.T) * last[bi] / vmax;
    g.fillStyle = BC[ALLB[bi]];
    g.fillRect(x, y, bw, F.B - y);
    g.fillStyle = '#555'; g.font = '11px sans-serif';
    g.fillText(last[bi].toFixed(1), x + 2, y - 3);
  }
  if (!any) emptyNote(g, F, '暂无数据…');
}

// 饼图：两板「最新值」占总量之比
function chartPie(cvId, key){
  const cv = document.getElementById(cvId);
  if (!cv || !showChart) return;
  const g = cv.getContext('2d'), W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  g.fillStyle = '#fff'; g.fillRect(0, 0, W, H);

  const last = ALLB.map(function(b){
    const s = seriesOf(b, key);
    return { b: b, v: s.length ? Math.max(0, s[s.length - 1].v) : 0 };
  });
  let total = 0;
  for (const s of last) total += s.v;

  g.fillStyle = '#9aa4b0'; g.font = '11px sans-serif';
  g.fillText('两板占比', 8, 14);

  if (total <= 0){
    g.fillStyle = '#999'; g.font = '12px sans-serif';
    g.fillText('暂无数据…', 20, H / 2 + 6);
    return;
  }

  const cx = W / 2 - 34, cy = H / 2 + 8, R = Math.min(W, H) / 2 - 26;
  let a0 = -Math.PI / 2;
  for (const s of last){
    const a1 = a0 + (s.v / total) * Math.PI * 2;
    g.beginPath();
    g.moveTo(cx, cy);
    g.arc(cx, cy, R, a0, a1);
    g.closePath();
    g.fillStyle = BC[s.b]; g.fill();
    g.strokeStyle = '#fff'; g.lineWidth = 1.5; g.stroke();
    a0 = a1;
  }
  let ly = 34;
  for (const s of last){
    const pct = s.v / total * 100;
    g.fillStyle = BC[s.b]; g.fillRect(W - 76, ly - 8, 8, 8);
    g.fillStyle = '#555'; g.font = '11px sans-serif';
    g.fillText('板' + s.b + ' ' + pct.toFixed(0) + '%', W - 64, ly);
    ly += 16;
  }
}

// 散点图：各板每个采样点画一个点（x = 采样序号，y = 数值）
function chartScatter(cvId, key){
  const cv = document.getElementById(cvId);
  if (!cv || !showChart) return;
  const g = cv.getContext('2d'), W = cv.width, H = cv.height;
  g.clearRect(0, 0, W, H);
  g.fillStyle = '#fff'; g.fillRect(0, 0, W, H);

  const ser = ALLB.map(function(b){ return seriesOf(b, key); });
  const rng = chartRange(ser), vmin = rng[0], vmax = rng[1];
  const F = chartFrame(g, W, H, vmin, vmax);
  chartLegend(g);

  let drew = 0;
  for (let bi = 0; bi < ALLB.length; bi++){
    const b = ALLB[bi], pts = ser[bi];
    if (!pts.length) continue;
    drew++;
    g.fillStyle = BC[b];
    for (let i = 0; i < pts.length; i++){
      const x = F.L + (F.R - F.L) * (pts.length > 1 ? i / (pts.length - 1) : 0.5);
      const y = F.B - (F.B - F.T) * (pts[i].v - vmin) / (vmax - vmin);
      g.beginPath(); g.arc(x, y, 3, 0, Math.PI * 2); g.fill();
    }
  }
  if (drew === 0) emptyNote(g, F, '暂无数据…');
}

function drawAll(){
  for (const c of CHARTS){
    const id = c.style + '_' + c.key;
    if (c.style === 'bar')          chartBar(id, c.key);
    else if (c.style === 'pie')     chartPie(id, c.key);
    else if (c.style === 'scatter') chartScatter(id, c.key);
    else                            chartLine(id, c.key, c.style);
  }
}

async function load(){
  try{
    const r = await fetch('/api/data');
    rowsAll = await r.json();
    const rows = applyView(rowsAll);
    const n1 = rowsAll.filter(function(d){ return d.board === 1; }).length;
    const n2 = rowsAll.filter(function(d){ return d.board === 2; }).length;
    document.getElementById('bar').textContent =
      '符合筛选 ' + rows.length + ' / 已取 ' + rowsAll.length + ' 条（板1 ' + n1 + ' · 板2 ' + n2 +
      '）· 刷新于 ' + new Date().toLocaleTimeString();

    /* ★ 分页：按筛选后的行数重算总页数，并夹紧当前页（数据在增长/清空都不越界）*/
    pageTotal = Math.max(1, Math.ceil(rows.length / pageSize));
    if (page > pageTotal - 1) page = pageTotal - 1;
    if (page < 0) page = 0;

    /* ★ 列表页才有 tbody；图形页没有 → 只画图。
       （两页共用同一份 JS，靠"元素在不在"分支，避免两套脚本各自漂移。）*/
    const tb = document.getElementById('tb');
    if (tb){
      tb.innerHTML = '';
      const start = page * pageSize;
      const slice = rows.slice(start, start + pageSize);
      for (const d of slice) tb.appendChild(makeRow(d));
      renderPager(rows.length, start, slice.length);
    }
    if (document.getElementById('charts')) drawAll(rows);
  }catch(e){
    document.getElementById('bar').innerHTML = '<span class="err">取数据失败：' + e + '</span>';
  }
}

/* ---- 采集状态条刷新（S 帧）：题1「web 页面显示采集开始或停止」---- */
function statText(o){
  if (!o || !o.online) return '未连接';
  if (o.state === 'START') return '采集进行中';
  if (o.state === 'STOP')  return '已停止';
  return '未知';
}
async function refreshStatus(){
  try{
    const r = await fetch('/api/status');
    const j = await r.json();
    const ms = j.master_state;
    const pill = document.getElementById('pill_all');
    if (pill){
      let txt = '未知', cls = 'unknown';
      if (ms === 'START'){ txt = '采集进行中'; cls = 'on'; }
      else if (ms === 'STOP'){ txt = '已停止'; cls = 'off'; }
      else if (ms === 'OFFLINE'){ txt = '板1 未连接'; cls = 'unknown'; }
      pill.textContent = txt;
      pill.className = 'pill ' + cls;
    }
    const bd = j.boards || {};
    const e1 = document.getElementById('bd1');
    const e2 = document.getElementById('bd2');
    if (e1) e1.textContent = '板1：' + statText(bd['1']);
    if (e2) e2.textContent = '板2：' + statText(bd['2']);
    /* ★ 身份（姓名 + 学号）一律**以板子的握手帧 H 为准**。
       原来：h1 里写死「郑鑫 · 202440700111」、who 只认板1 —— 换人只要改
       app_id.h 的 STU_WHO，板子会发新学号，但网页永远显示旧的那一份。
       现在：取第一个已握手的板填 h1，并标注两板是否一致。*/
    const stu = j.stu || {};
    const known = ['1', '2'].filter(function(k){
      return stu[k] && stu[k].name && stu[k].name !== '?';
    });
    const who  = document.getElementById('who');
    const sign = document.getElementById('sign');
    if (known.length) {
      const first = stu[known[0]];
      const same  = known.every(function(k){
        return stu[k].name === first.name && stu[k].sid === first.sid;
      });
      if (who)  who.textContent  = first.name + '　' + first.sid +
                                   (known.length > 1 ? '（' + known.length + ' 板已确认）' : '');
      if (sign) sign.textContent = first.name + ' · ' + first.sid +
                                   (same ? '' : ' ⚠ 两板不一致');
    }
  }catch(e){}
}

/* ---- 分页控制 ---- */
function renderPager(total, start, n){
  document.getElementById('p_info').textContent =
    '第 ' + (total === 0 ? 0 : page + 1) + ' / ' + pageTotal + ' 页 · 本页 ' + n +
    ' 条' + (total > 0 ? '（' + (start + 1) + '~' + (start + n) + ' / 共 ' + total + '）' : '');
  document.getElementById('p_first').disabled = (page <= 0);
  document.getElementById('p_prev').disabled  = (page <= 0);
  document.getElementById('p_next').disabled  = (page >= pageTotal - 1);
  document.getElementById('p_last').disabled  = (page >= pageTotal - 1);
}

function gotoPage(p){
  if (p < 0) p = 0;
  if (p > pageTotal - 1) p = pageTotal - 1;
  page = p;
  load();
}

function onPageSize(){
  pageSize = parseInt(document.getElementById('p_size').value, 10) || 50;
  page = 0;                     /* 改每页条数：回到第一页，避免页码错位 */
  load();
}

function jumpTo(){
  const v = parseInt(document.getElementById('p_jump').value, 10);
  if (!isNaN(v)) gotoPage(v - 1);
}

function bcell(b){ return '<span class="b' + b + '">板' + b + '</span>'; }
/* 板端神经网络的判定（协议 v7 D 帧尾部）：未推理时显示 "—" */
function aiCell(d){
  if (d.ai_cls && d.ai_cls > 0){
    const hit = (d.ai_cls === d.board);
    return '<span class="' + (hit ? 'ok' : 'alm') + '">判为板' + d.ai_cls +
           (d.ai_conf !== null && d.ai_conf !== undefined ? '（' + d.ai_conf + '%）' : '') +
           (hit ? ' ✔' : ' ✘') + '</span>';
  }
  return '—';
}

function makeRow(d){
  const tr = document.createElement('tr');
  if (editing === d.id){
    tr.innerHTML =
      '<td>' + d.id + '</td><td>' + bcell(d.board) + '</td><td>' + d.seq + '</td>' +
      '<td><input class="ed" id="e_t" value="' + d.temp + '"></td>' +
      '<td><input class="ed" id="e_h" value="' + d.humi + '"></td>' +
      '<td><input class="ed" id="e_l" value="' + d.light + '"></td>' +
      '<td><input class="ed" id="e_c" value="' + d.eco2 + '"></td>' +
      '<td><input class="ed" id="e_v" value="' + d.tvoc + '"></td>' +
      '<td><select id="e_s">' +
        '<option value="OK"' + (d.status==='OK'?' selected':'') + '>OK</option>' +
        '<option value="ALM"' + (d.status==='ALM'?' selected':'') + '>ALM</option>' +
      '</select></td>' +
      '<td>' + d.sample_time + '</td><td>' + d.recv_time + '</td>' +
      '<td>' + aiCell(d) + '</td>' +
      '<td><button onclick="save(' + d.id + ')">保存</button> ' +
          '<button onclick="cancel()">取消</button></td>';
  } else {
    tr.innerHTML =
      '<td>' + d.id + '</td><td>' + bcell(d.board) + '</td><td>' + d.seq + '</td><td>' + d.temp + '</td>' +
      '<td>' + d.humi + '</td><td>' + d.light + '</td>' +
      '<td>' + d.eco2 + '</td><td>' + d.tvoc + '</td>' +
      '<td class="' + (d.status==='ALM'?'alm':'ok') + '">' + d.status + '</td>' +
      '<td>' + d.sample_time + '</td><td>' + d.recv_time + '</td>' +
      '<td>' + aiCell(d) + '</td>' +
      '<td><button onclick="startEdit(' + d.id + ')">编辑</button> ' +
          '<button onclick="delRow(' + d.id + ')">删除</button></td>';
  }
  return tr;
}
function startEdit(id){ editing = id; load(); }
function cancel(){ editing = null; load(); }
async function save(id){
  const body = {id:id,
                temp:document.getElementById('e_t').value,
                humi:document.getElementById('e_h').value,
                light:document.getElementById('e_l').value,
                eco2:document.getElementById('e_c').value,
                tvoc:document.getElementById('e_v').value,
                status:document.getElementById('e_s').value};
  const r = await fetch('/api/update', {method:'POST', body:JSON.stringify(body)});
  if (!r.ok){ alert('保存失败：' + await r.text()); }
  editing = null; load();
}
async function delRow(id){
  if (!confirm('确定删除 id=' + id + ' 吗？（数据库里也会删）')) return;
  await fetch('/api/delete', {method:'POST', body:JSON.stringify({id:id})});
  load();
}
async function clearAll(){
  if (!confirm('确定清空整张 lab7_data 表吗？（不可恢复，自增也会归零）')) return;
  const r = await fetch('/api/clear', {method:'POST', body:'{}'});
  const j = await r.json();
  alert('已清空，删除 ' + j.n + ' 行');
  editing = null; load();
}
setInterval(function(){ if (editing === null) load(); }, 1000);
setInterval(refreshStatus, 2000);
(function(){                                   /* 当前页高亮 */
  const a = document.getElementById(KIND === 'list' ? 'nav_list' : 'nav_chart');
  if (a) a.className = 'on';
})();
load();
refreshStatus();
</script>
</body></html>
"""


# ---- 图表容器：**每种数据一种风格，且互不重复**（5 参数 ↔ 5 种风格）----
_CHART_PARAMS = [
    ("temp",  "温度 ℃",   "line"),      # 折线：看趋势
    ("humi",  "湿度 %RH", "bar"),       # 柱状：两板对比
    ("light", "光敏 %",   "area"),      # 面积：叠加
    ("eco2",  "CO₂ ppm",  "pie"),       # 饼图：两板占比
    ("tvoc",  "TVOC ppb", "scatter"),   # 散点：分布/离群
]
_STYLE_NAME = {
    "line": "折线图", "bar": "柱状图", "area": "面积图",
    "pie": "饼图", "scatter": "散点图",
}


def _charts_html():
    """图形页内容：每种数据一种风格且互不重复（折线/柱状/面积/饼/散点）。"""
    cells = []
    for key, label, style in _CHART_PARAMS:
        cid = style + "_" + key
        cells.append(
            '<div class="wrap"><h2>%s · %s</h2>'
            '<canvas id="%s" width="300" height="170" '
            'style="width:300px;height:170px"></canvas></div>'
            % (label, _STYLE_NAME[style], cid))
    return ('<h3 class="sec">各参数图形 · 每种数据一种风格'
            '（折线 / 柱状 / 面积 / 饼 / 散点，互不重复）</h3>'
            '<div class="grid">%s</div>' % "".join(cells))


# ============================================================================
#  ★ 题1 要求"分 2 页"：把上面的模板切成两个页面。
#    _TPL 里用 <!--LIST_START-->…<!--LIST_END--> 和 <!--CHART_START-->…<!--CHART_END-->
#    圈出两块互斥内容；建页时把不需要的那块**整段切掉**，
#    于是两个页面共用同一份 CSS / JS —— 改一处两页同时生效，不会各自漂移。
# ============================================================================
def _cut(html, a, b):
    i = html.index(a)
    j = html.index(b) + len(b)
    return html[:i] + html[j:]


def _mk_page(kind, name):
    html = _TPL
    if kind == "list":
        html = _cut(html, "<!--CHART_START-->", "<!--CHART_END-->")
    else:
        html = _cut(html, "<!--LIST_START-->", "<!--LIST_END-->")
    html = (html.replace("<!--LIST_START-->", "").replace("<!--LIST_END-->", "")
                .replace("<!--CHART_START-->", "").replace("<!--CHART_END-->", "")
                .replace("__KIND__", kind).replace("__PAGE_NAME__", name)
                .replace("<!--CHARTS-->", _charts_html()))
    return html


PAGE_LIST = _mk_page("list", "记录列表")
PAGE_CHART = _mk_page("chart", "图形")


class WebHandler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = urlparse(self.path).path
        if p in ("/", "/index.html"):
            self._send(200, PAGE_LIST, "text/html; charset=utf-8")     # 页① 记录列表（分页）
        elif p == "/chart":
            self._send(200, PAGE_CHART, "text/html; charset=utf-8")    # 页② 图形
        elif p == "/api/data":
            self._send(200, json.dumps(fetch_rows(), ensure_ascii=False))
        elif p == "/api/status":
            self._send(200, json.dumps(collect_status(), ensure_ascii=False))
        else:
            self._send(404, "{}")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(n).decode("utf-8") if n else "{}"
        try:
            req = json.loads(raw)
        except Exception:
            req = {}
        p = urlparse(self.path).path
        try:
            if p == "/api/update":
                # ★ 原来只传了 5 个参数（少了 eco2 / tvoc），而 update_row 的签名要 7 个
                #   → 网页「保存编辑」一直抛 TypeError（这个 bug 是 2026-10-19 做实验八时
                #   才发现的，两个仓库一起修）。
                update_row(int(req["id"]), float(req["temp"]), float(req["humi"]),
                           int(req["light"]), int(req.get("eco2", 0)),
                           int(req.get("tvoc", 0)), str(req["status"]))
                self._send(200, '{"ok":true}')
            elif p == "/api/delete":
                delete_row(int(req["id"]))
                self._send(200, '{"ok":true}')
            elif p == "/api/clear":
                self._send(200, json.dumps({"ok": True, "n": clear_all()}))
            else:
                self._send(404, '{"ok":false}')
        except Exception as e:
            self._send(500, json.dumps({"ok": False, "err": str(e)}))

    def log_message(self, *a):
        pass                        # 别把每条 HTTP 请求刷到控制台


# ★ 每块板"当前"那条连接。
#   板端 ESP-01S 掉线重连时，旧连接可能还挂着一小会儿 —— 两条连接同时
#   往库里写，seq 会重复、数据会翻倍（实测日志里 15/16 帧重复出现）。
#   认出板号后把同板的旧连接踢掉，谁新谁活。
_CUR_CONN = {}


def handle_client(conn, addr):
    """一条连接一个线程。板号在收到带 board 字段的帧后才认出来（协议 v5）。"""
    tag = "?"
    bnum = None         # 认出板号后填；被踢掉的旧连接用它决定日志要不要静默
    pr("[连接] %s:%d 已连上" % (addr[0], addr[1]))
    buf = b""
    t0 = time.time()
    n_rx = 0
    quiet_hi = False
    conn.settimeout(5.0)        # ★ 5s 一个字节都没来就提示一次（别让用户干等）
    try:
        while True:
            try:
                data = conn.recv(1024)
            except socket.timeout:
                if (not quiet_hi) and (n_rx == 0):
                    quiet_hi = True
                    pr("[提示] %s:%d 连上 %.0f 秒但**一个字节都没收到** —— TCP 通了、应用层没发。"
                       "多半是板端 WiFi 状态机没走到 READY（看板子状态页 WiFi 那栏 / 串口敲 w）"
                       % (addr[0], addr[1], time.time() - t0))
                continue
            if not data:
                break
            n_rx += len(data)
            buf += data
            while b"\n" in buf:                 # 按行拆，中文不会半个字
                line, buf = buf.split(b"\n", 1)
                text = line.decode("gbk", "replace").strip()
                if not text:
                    continue
                # 认出板号后给这条连接打个标签（日志好读）
                # ★ H 帧的板号在最后字段，D/C 帧在 p[1] —— 两种都要认
                if tag == "?":
                    p = text.split(",")
                    bnum = None
                    if text.startswith("H,") and len(p) >= 4 and p[3] in ("1", "2", "3"):
                        bnum = int(p[3])
                    elif len(p) >= 2 and p[1] in ("1", "2", "3"):
                        bnum = int(p[1])
                    if bnum in BOARDS:
                        tag = BOARDS[bnum]["name"]
                        # ★ 同板重连：踢掉旧连接（否则两条线程同时入库 → 数据翻倍）
                        old = _CUR_CONN.get(bnum)
                        if old is not None and old is not conn:
                            try:
                                old.close()
                                pr("[连接] %s 重连 —— 已主动关闭旧连接" % tag)
                            except Exception:
                                pass
                        _CUR_CONN[bnum] = conn
                        pr("[连接] %s:%d 识别为 %s" % (addr[0], addr[1], tag))
                on_line(text)
    except Exception as e:
        # ★ 被新连接踢掉的旧连接，recv() 会抛 WinError 10038（socket 已被我们关掉）。
        #   那不是故障，别当错误打出来吓人。
        if bnum is not None and _CUR_CONN.get(bnum) is not conn:
            pass
        else:
            pr("[错误] %s" % e)
    finally:
        try:
            conn.close()
        except Exception:
            pass
        # 只有"当前连接"才从表里摘掉，别把后来的新连接误删
        for _b, _c in list(_CUR_CONN.items()):
            if _c is conn:
                _CUR_CONN.pop(_b, None)
        pr("[断开] %s:%d（%s）  本机累计收到 %d 帧/共 %d 字节"
           % (addr[0], addr[1], tag, rx_count, n_rx))


def main():
    # 网页服务（后台线程）：页① 记录列表（分页）  页② 图形
    try:
        web = ThreadingHTTPServer(("0.0.0.0", WEB_PORT), WebHandler)
        threading.Thread(target=web.serve_forever, daemon=True).start()
        print("[网页] ① 记录列表 http://127.0.0.1:%d/    ② 图形 http://127.0.0.1:%d/chart"
              % (WEB_PORT, WEB_PORT))
    except Exception as e:
        print("[网页] 启动失败：%s" % e)

    # ★★ 实验七：**默认保留**库里的数据，只在明确要求时才清空。
    #    为什么跟实验五/六反过来：实验五/六每次演示都从干净表开始，清空是对的；
    #    但实验七的表是**题2 的数据集**（要攒到两板各 ≥100 条），
    #    顺手重启一下上位机就把数据删了 = 白采一轮（真踩过：一次删掉 58 行）。
    #      要清空：STM32_DB_CLEAR=1
    #      （兼容旧习惯：STM32_DB_KEEP=1 现在等价于默认行为，保留）
    if _db is not None:
        if os.environ.get("STM32_DB_CLEAR", "") == "1":
            print("[DB] STM32_DB_CLEAR=1 → 清空 lab7_data：删除 %d 行" % clear_all())
        else:
            print("[DB] 保留 lab7_data 里已有的 %d 行（要清空就设 STM32_DB_CLEAR=1）"
                  % db_count())

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(4)

    print_header()

    try:
        while True:
            conn, addr = srv.accept()
            # 一连接一线程：旧残骸不挡新连接（两板各一条；重连也照收）
            threading.Thread(target=handle_client, args=(conn, addr),
                             daemon=True).start()
    except KeyboardInterrupt:
        pr("\n[退出] Ctrl+C，关闭监听。")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
