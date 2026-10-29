#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""冒烟用的最小 PC 监听器（零依赖，只用标准库）。

用途：合并后冒烟时，把"板1 → PC（link0）"这条链也拉进来跑，验证：
  1) 板1 能连上 PC:8080（link0=OK）；
  2) D 数据帧的"序号"连续（有洞 = 板1→PC 方向丢帧）；
  3) 多流量（D 帧 + 按键 C 帧 + K 心跳同时跑）下两板读数仍然干净。

用法（PC 上，热点已开、防火墙放行 8080）：
    python pc_smoke.py            # 监听 0.0.0.0:8080
    python pc_smoke.py 8080       # 指定端口
Ctrl+C 结束并打总结。

帧格式（当前阶段，板1 → PC/板2）：
    H,<姓名>,<学号>                                  连上时发一次
    D,<up_ms>,<序号>,<温度>.<一位>,<湿度>.<一位>,<光>,<OK|ALM>   约 1 帧/秒
    P,HELLO,1 / P,ACK,1                              串口 p 命令的手工测试帧
    （K 心跳只发板2，PC 这边看不到 —— 那是两板日志的事）

冒烟通过线：D 序号无洞（或洞 ≤1%）、帧率 ≈1/s、无长时间静默（>3s 无帧）。
"""

import socket
import sys
import time
import functools

# 行行都冲出去：不然重定向到文件时会因为块缓冲看不到实时输出
print = functools.partial(print, flush=True)

HOST = "0.0.0.0"


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, port))
    srv.listen(4)
    srv.settimeout(0.5)
    print("[PC] listening on %s:%u  (Ctrl+C to stop)" % (HOST, port))

    conns = []
    stat = {"frames": 0, "H": 0, "D": 0, "C": 0, "other": 0}
    seq_last = 0
    gaps = []
    tw = time.time()
    sw = dict(stat)
    last_rx = time.time()

    try:
        while True:
            try:
                c, addr = srv.accept()
                c.settimeout(0.2)
                conns.append(c)
                print("[PC] + client %s:%u  (共 %u 条连接)" % (addr[0], addr[1], len(conns)))
            except socket.timeout:
                pass

            for c in list(conns):
                try:
                    data = c.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    conns.remove(c)
                    print("[PC] - client closed (err)")
                    continue
                if not data:
                    conns.remove(c)
                    print("[PC] - client closed  (共 %u 条连接)" % len(conns))
                    continue
                last_rx = time.time()
                for line in data.decode("utf-8", "replace").splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    kind = line.split(",", 1)[0]
                    stat["frames"] += 1
                    if kind in stat:
                        stat[kind] += 1
                    else:
                        stat["other"] += 1
                    print("[PC] %s" % line)
                    if kind == "D":
                        f = line.split(",")
                        if len(f) > 3 and f[3].isdigit():     # v5: D,<board>,<up_ms>,<seq>,...
                            n = int(f[3])
                            if seq_last and n > seq_last + 1:
                                gaps.append((seq_last + 1, n - 1))
                                print("[PC] !! D 序号空洞 %u..%u (w=%u)"
                                      % (seq_last + 1, n - 1, n - seq_last - 1))
                            seq_last = n

            now = time.time()
            if now - tw >= 10.0:
                win = now - tw
                d = stat["frames"] - sw["frames"]
                print("[PC] win=%.0fs frames=%u (%.1f/s) | H=%u D=%u C=%u other=%u "
                      "| D_last=%u D_gaps=%u"
                      % (win, d, d / win, stat["H"], stat["D"], stat["C"],
                         stat["other"], seq_last, len(gaps)))
                if now - last_rx > 3.0:
                    print("[PC] !! 已经 %.0fs 没收到任何帧（链路可能停了）" % (now - last_rx))
                tw = now
                sw = dict(stat)
    except KeyboardInterrupt:
        pass
    finally:
        print("\n[PC] ==== 总结 ====")
        print("[PC] frames=%u  H=%u  D=%u  C=%u  other=%u  D_last=%u  D_gaps=%u"
              % (stat["frames"], stat["H"], stat["D"], stat["C"],
                 stat["other"], seq_last, len(gaps)))
        for a, b in gaps:
            print("[PC]   gap %u..%u (w=%u)" % (a, b, b - a + 1))
        print("[PC] 判定：D 序号无洞 / 洞 ≤1%%、帧率 ≈1/s、无 >3s 静默 = 通过")


if __name__ == "__main__":
    main()
