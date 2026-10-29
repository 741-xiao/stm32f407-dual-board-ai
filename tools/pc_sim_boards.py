#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两块 STM32F407 的**假板**模拟器（零依赖，只用标准库）。

用途：没有板子在手上时，让上位机（pc/upper_computer.py）跑起来做端到端验证——
      网页 2 页、分页、采集状态条（S 帧）、AI 判定列，都能先离线跑通。

它做的事就是**冒充两块板**各开一条 TCP：
    连 PC:8080 → 发 H 握手 → 按 1Hz 发 D 数据帧 → 按时间轴发 S 启停帧

时间轴（默认，可用 --seq 关掉）：
    t=0s   两块板各发 H,<姓名>,<学号>,<板号>
    t=0s   板2 发 S,2,START（板2 上电即采）
    t=0s   板1 发 S,1,STOP （板1 要按键才开始）
    t=5s   板1 发 S,1,START（模拟"板1 按键1"）
    t=25s  板1 发 S,1,STOP （模拟"板1 按键2"）
    t=25s  板1 停止发 D 帧，板2 继续

★ 两块板的数据是**故意造成可分**的（板1 偏暗／低温，板2 偏亮／高温），
  这样题2 的神经网络流水线（可分性检验→训练→推理）可以先用这套模拟数据
  把链路打通、把脚本调好；真板数据一到，换数据源即可。
  —— 模拟数据只能用来"调通流程"，**不能**当题2 的最终训练集写进报告。

用法：
    python pc_sim_boards.py                        # 连 127.0.0.1:8080
    python pc_sim_boards.py 192.168.137.1 8080     # 指定 PC 热点地址
    python pc_sim_boards.py --secs 60              # 跑 60 秒后退出
    python pc_sim_boards.py --ai                   # D 帧尾部带上 AI 判定字段（模拟阶段5）
    python pc_sim_boards.py --no-seq               # 不发 S 帧（只测数据链路）

没有 MySQL、没有板子，也想先把题2 的 AI 流水线跑通时（**只调流程，别当最终训练集**）：
    python pc_sim_boards.py --steady --interval 0.05 --secs 8 --csv ai/dataset/sim.csv
"""

import argparse
import csv
import math
import os
import random
import socket
import sys
import threading
import time
from datetime import datetime

# 姓名/学号必须与固件 app_id.h 一致，否则网页署名行显示的是模拟值
STU_NAME = "郑鑫"
STU_ID = "202440700111"

# 每块板的"环境"：板1 偏暗偏凉，板2 偏亮偏暖（制造可分性）
ENV = {
    1: dict(temp=24.5, humi=52.0, light=1200, eco2=500, tvoc=20),
    2: dict(temp=27.5, humi=58.0, light=3300, eco2=760, tvoc=34),
}

# 板1 的启停时间轴（秒）：(时刻, START/STOP)；板2 上电即采、不停
B1_TIMELINE = [(5.0, "START"), (25.0, "STOP")]

# ---- --csv：直接落数据集（和 tools/export_dataset.py 的列完全一致）----
#   目的是**没有 MySQL、没有板子**也能把题2 的流水线（可分性检验 → 训练）先跑通。
#   ★ 模拟数据只能用来调流程，**不能当题2 的最终训练集写进报告**。
CSV_COLS = ["board", "seq", "up_ms", "temp", "humi", "light", "eco2", "tvoc",
            "status", "sample_time", "recv_time", "ai_cls", "ai_conf"]
_csv_lock = threading.Lock()


def csv_write(path, row):
    with _csv_lock:
        d = os.path.dirname(os.path.abspath(path))
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)      # ★ 目录不存在就建：
            #   别让"写 CSV"这种附带功能把采集线程搞崩（踩过一次，线程静默死掉）
        new = not os.path.exists(path)
        with open(path, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(CSV_COLS)
            w.writerow(row)


def _send(sock, text):
    sock.sendall((text + "\r\n").encode("gbk", "replace"))


def _frame_d(board, seq, up_ms, with_ai):
    """组一条 D 帧（协议 v7）：D,<board>,<up_ms>,<seq>,<t>,<h>,<light>,<eco2>,<tvoc>,<OK|ALM>[,<ai_cls>,<ai_conf>]"""
    e = ENV[board]
    # 缓慢漂移 + 小噪声：别是一条直线，图形页才看得出东西
    ph = math.sin(up_ms / 20000.0 + board)
    temp = round(e["temp"] + ph * 0.8 + random.uniform(-0.2, 0.2), 1)
    humi = round(e["humi"] + ph * 1.2 + random.uniform(-0.3, 0.3), 1)
    light = int(e["light"] + ph * 120 + random.uniform(-25, 25))
    eco2 = int(e["eco2"] + ph * 30 + random.uniform(-8, 8))
    tvoc = int(e["tvoc"] + ph * 3 + random.uniform(-2, 2))
    txt = "D,%d,%d,%d,%.1f,%.1f,%d,%d,%d,OK" % (
        board, up_ms, seq, temp, humi, light, eco2, tvoc)
    if with_ai:
        # 模拟板端推理：正常应判成本板（板1→1、板2→2），置信度 85~99
        txt += ",%d,%d" % (board, random.randint(85, 99))
    return txt, (temp, humi, light, eco2, tvoc)

def board_run(b, host, port, args, t0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((host, port))
    except Exception as e:
        print("[板%d] 连不上 %s:%d —— %s" % (b, host, port, e))
        return
    print("[板%d] 已连上 %s:%d" % (b, host, port))

    _send(sock, "H,%s,%s,%d" % (STU_NAME, STU_ID, b))
    if args.seq and b == 2:
        _send(sock, "S,2,START")          # 板2 上电即采（固件行为）
    if args.seq and b == 1:
        _send(sock, "S,1,STOP")           # 板1 上电不采，等按键

    seq = 0
    running = (b == 2) or (b == 1 and args.steady)   # steady：板1 也立刻开采（造数据集用）
    if args.steady and b == 1 and args.seq:
        _send(sock, "S,1,START")
    tl = [] if args.steady else ([t for t in B1_TIMELINE] if (b == 1 and args.seq) else [])
    nxt = 0
    while True:
        el = time.time() - t0
        if args.secs and el > args.secs:
            break
        # 该发 S 帧了吗
        while nxt < len(tl) and el >= tl[nxt][0]:
            st = tl[nxt][1]
            running = (st == "START")
            _send(sock, "S,1,%s" % st)
            print("[板1] 发 S,1,%s（模拟按键）" % st)
            nxt += 1
        if running:
            seq += 1
            up_ms = int((time.time() - t0) * 1000)
            txt, vals = _frame_d(b, seq, up_ms, args.ai)
            try:
                _send(sock, txt)
            except Exception as e:
                print("[板%d] 发送失败：%s" % (b, e))
                break
            if args.csv:                       # 直接落数据集（给题2 的流水线用）
                ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                csv_write(args.csv, [b, seq, up_ms, vals[0], vals[1], vals[2],
                                     vals[3], vals[4], "OK", ts, ts, "", ""])
            if seq <= 3 or seq % 20 == 0:
                print("[板%d] %s" % (b, txt))
        time.sleep(args.interval)
    try:
        sock.close()
    except Exception:
        pass
    print("[板%d] 结束" % b)


def main():
    ap = argparse.ArgumentParser(description="实验七 假板模拟器（两块板各一条 TCP）")
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("port", nargs="?", type=int, default=8080)
    ap.add_argument("--secs", type=float, default=0, help="跑多少秒后退出（0=一直跑）")
    ap.add_argument("--interval", type=float, default=1.0, help="采样周期（秒），默认 1.0")
    ap.add_argument("--ai", action="store_true", help="D 帧尾部带 AI 判定字段（模拟阶段5）")
    ap.add_argument("--no-seq", dest="seq", action="store_false", help="不发 S 启停帧")
    ap.add_argument("--csv", default="", help="同时把样本落成 CSV（题2 数据集；列与 export_dataset.py 一致）")
    ap.add_argument("--steady", action="store_true",
                    help="两板都立刻开采且不停（造数据集用；默认板1 在 t=5s 开、t=25s 停）")
    ap.set_defaults(seq=True)
    args = ap.parse_args()

    print("=" * 78)
    print("  实验七 假板模拟器 → %s:%d   周期 %.2fs   %s   %s"
          % (args.host, args.port, args.interval,
             "带 AI 字段" if args.ai else "无 AI 字段",
             "发 S 帧" if args.seq else "不发 S 帧"))
    if args.secs:
        print("  运行 %.0f 秒后退出" % args.secs)
    print("=" * 78)

    t0 = time.time()
    ts = [threading.Thread(target=board_run, args=(b, args.host, args.port, args, t0), daemon=True)
          for b in (1, 2)]
    for t in ts:
        t.start()
    try:
        for t in ts:
            t.join()
    except KeyboardInterrupt:
        print("\n[退出] Ctrl+C")
    return 0


if __name__ == "__main__":
    sys.exit(main())
