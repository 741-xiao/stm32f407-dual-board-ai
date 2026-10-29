#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""题2 的神经网络训练：两板数据 → 判「这条数据来自哪块板」。

路线（贴题目给的 CSDN 教程：Keras 训练 → X-CUBE-AI 转 C → 板上推理）：
    ai/dataset/*.csv
      → z-score 标准化（mean/std 由**训练集**算出，同时导出固件用的 ai_norm.h）
      → 小 MLP：5 → 16 → 16 → 2 (softmax)      ← 402 个参数，约 1.6 KB
      → 训练 + 早停 + 混淆矩阵
      → 导出 model.h5 / model.keras（X-CUBE-AI 首选）
               model.onnx        （兜底：X-CUBE-AI 对 Keras 版本挑剔时改用它）
               ai_norm.h         （★ 板上必须用**同一套** mean/std 归一化）
               ai_test10.h       （题2 要求的「任取 10 条后续记录」，留给板端逐条判定）

依赖：numpy + tensorflow(keras) +（可选）matplotlib；onnx 仅在做 ONNX 兜底时需要
    pip install numpy tensorflow matplotlib onnx

用法：
    python tools/train_nn.py                        # 读 ai/dataset/lab7.csv
    python tools/train_nn.py ai/dataset/run2.csv --epochs 400 --hidden 32,16
    python tools/train_nn.py --holdout-per-board 5  # 每板留最后 5 条做"后续数据"（默认）
"""

import argparse
import csv
import json
import os
import sys
from datetime import datetime

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FEATS = ["temp", "humi", "light", "eco2", "tvoc"]
FEAT_CN = {"temp": "温度", "humi": "湿度", "light": "光敏", "eco2": "CO2", "tvoc": "TVOC"}


def log(*a):
    print(*a, flush=True)


def load_csv(path):
    """按行读；返回 X(原始值)、y(0=板1,1=板2)、raw(原始 dict 列表，留出样本要用)"""
    X, y, raw = [], [], []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                v = [float(r[k]) for k in FEATS]
            except (ValueError, KeyError):
                continue
            X.append(v)
            y.append(0 if int(r["board"]) == 1 else 1)
            raw.append(r)
    return np.asarray(X, dtype="float32"), np.asarray(y, dtype="int64"), raw


def main():
    ap = argparse.ArgumentParser(description="实验七 题2：训练判板号的神经网络")
    ap.add_argument("csv", nargs="?", default=os.path.join("ai", "dataset", "lab7.csv"))
    ap.add_argument("--epochs", type=int, default=300)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--hidden", default="16,16", help="隐藏层，逗号分隔（默认 16,16）")
    ap.add_argument("--lr", type=float, default=0.01)
    ap.add_argument("--holdout-per-board", type=int, default=5,
                    help="每板留出最后 N 条做「后续采集数据」（默认 5 → 共 10 条，贴题面）")
    ap.add_argument("--eval-csv", default="",
                    help="★ 额外评估集（如**演示环境**下采的 10cm 数据）→ 直接测出模型"
                         "在演示现场行不行；不填则跳过")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--features", default="temp,humi",
                    help="用哪几个通道判板（逗号分隔，顺序即网络输入顺序）。"
                         "默认 temp,humi —— 实测光敏同一块板自己能漂 27%%（开灯/挡光），"
                         "把模型带偏过（板1 被判成板2）；CO2 只有 0.20σ、TVOC 0.86σ，也帮不上。"
                         "可选：temp,humi,light,eco2,tvoc")
    args = ap.parse_args()

    use = [f.strip() for f in args.features.split(",") if f.strip()]
    for f in use:
        if f not in FEATS:
            log("✘ 未知特征 '%s'，可选：%s" % (f, "/".join(FEATS)))
            return 2
    if len(use) < 2:
        log("✘ 至少要两个特征，否则没法判（当前：%s）" % ",".join(use))
        return 2

    path = args.csv if os.path.isabs(args.csv) else os.path.join(ROOT, args.csv)
    if not os.path.exists(path):
        log("✘ 找不到 %s\n  先跑：python tools/export_dataset.py" % path)
        return 2

    try:
        import tensorflow as tf
        from tensorflow import keras
    except ImportError as e:
        log("✘ 缺依赖：%s\n  pip install tensorflow" % e)
        return 2

    np.random.seed(args.seed)
    tf.random.set_seed(args.seed)

    X_all, y, raw = load_csv(path)
    idx = [FEATS.index(f) for f in use]
    X = X_all[:, idx]                    # 只把选中的通道喂给网络
    log("=" * 74)
    log("  训练判板号模型   %s" % os.path.relpath(path, ROOT))
    log("  共 %d 条（板1 %d / 板2 %d）" % (len(y), (y == 0).sum(), (y == 1).sum()))
    log("  输入通道：%s（%d 维）" % (" / ".join(FEAT_CN[f] for f in use), len(use)))
    log("=" * 74)
    if (y == 0).sum() < 20 or (y == 1).sum() < 20:
        log("✘ 每板至少 20 条起训（题面要求 ≥100）。先把数据采够。")
        return 1

    # ---- 1) 留出"后续采集的数据"（每板最后 N 条，按 CSV 顺序=时间顺序）----
    ho = max(1, min(args.holdout_per_board, (y == 0).sum() // 4, (y == 1).sum() // 4))
    idx_ho = []
    for b in (0, 1):
        idx = np.where(y == b)[0]
        idx_ho.extend(list(idx[-ho:]))
    idx_ho = np.asarray(sorted(idx_ho))
    mask = np.ones(len(y), dtype=bool)
    mask[idx_ho] = False
    X_tr_all, y_tr_all = X[mask], y[mask]
    X_ho_all = X_all[idx_ho]          # ai_test10.h 用（保留全部原始列，固件按 AI_NORM_N 取前几列）
    X_ho     = X[idx_ho]              # 模型用（只含选中的通道）
    y_ho, raw_ho = y[idx_ho], [raw[i] for i in idx_ho]
    log("\n留出「后续采集」样本 %d 条（每板 %d 条，不参与训练）：" % (len(idx_ho), ho))
    for k, i in enumerate(idx_ho):
        log("  #%-2d 板%d  seq=%-4s  t=%-5s h=%-5s light=%-5s eco2=%-5s tvoc=%-4s"
            % (k + 1, y[i] + 1, raw[i].get("seq", "?"), raw[i]["temp"], raw[i]["humi"],
               raw[i]["light"], raw[i]["eco2"], raw[i]["tvoc"]))

    # ---- 2) 标准化（mean/std 只由训练集算 → 导出给固件复用）----
    mean = X_tr_all.mean(axis=0)
    std = X_tr_all.std(axis=0)
    std[std < 1e-6] = 1.0
    Xn = (X - mean) / std

    # 分层切训练/验证（8:2）
    rng = np.random.default_rng(args.seed)
    tr_idx, va_idx = [], []
    for b in (0, 1):
        idx = np.where(y_tr_all == b)[0]
        rng.shuffle(idx)
        cut = int(len(idx) * 0.8)
        tr_idx.extend(list(idx[:cut]))
        va_idx.extend(list(idx[cut:]))
    tr_idx, va_idx = np.asarray(sorted(tr_idx)), np.asarray(sorted(va_idx))
    Xtr, ytr = Xn[mask][tr_idx], y_tr_all[tr_idx]
    Xva, yva = Xn[mask][va_idx], y_tr_all[va_idx]
    log("\n训练集 %d 条 / 验证集 %d 条" % (len(ytr), len(yva)))

    # ---- 3) 建模型 ----
    hid = [int(v) for v in args.hidden.split(",") if v.strip()]
    m = keras.Sequential(name="board_clf")
    m.add(keras.layers.Input(shape=(len(use),), name="in"))
    for i, h in enumerate(hid):
        m.add(keras.layers.Dense(h, activation="relu", name="fc%d" % (i + 1)))
    m.add(keras.layers.Dense(2, activation="softmax", name="out"))
    m.compile(optimizer=keras.optimizers.Adam(args.lr),
              loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    m.summary(print_fn=lambda s: log("  " + s))
    n_par = int(np.sum([np.prod(w.shape) for w in m.get_weights()]))
    log("  参数量 %d ≈ %.2f KB（float32）" % (n_par, n_par * 4 / 1024.0))

    # ---- 4) 训练 ----
    log("\n开始训练（早停 patience=40，最多 %d 轮）…" % args.epochs)
    hist = m.fit(Xtr, ytr, validation_data=(Xva, yva),
                 epochs=args.epochs, batch_size=args.batch, verbose=0,
                 callbacks=[keras.callbacks.EarlyStopping(
                     monitor="val_loss", patience=40, restore_best_weights=True)])
    ep = len(hist.history["loss"])
    tr_acc = hist.history["accuracy"][-1]
    va_acc = hist.history["val_accuracy"][-1]
    log("  训练 %d 轮后：训练准确率 %.4f  验证准确率 %.4f" % (ep, tr_acc, va_acc))

    # ---- 5) 评估（混淆矩阵自己算，不依赖 sklearn —— 少一个装不上的风险）----
    p_va = m.predict(Xva, verbose=0)
    yhat_va = p_va.argmax(axis=1)
    cm = np.zeros((2, 2), dtype=int)
    for t, q in zip(yva, yhat_va):
        cm[int(t)][int(q)] += 1
    acc_va = float((yhat_va == yva).mean())

    # ---- 6) 留出 10 条的推理（题2 的"任取10条"演示）----
    Xho_n = (X_ho - mean) / std
    p_ho = m.predict(Xho_n, verbose=0)

    # ---- 主导特征（G7「哪个传感器」的落地口径）：**输入梯度显著性** ----
    #   |∂(正确类别的概率)/∂x_i| 越大 → 输出对这个通道越敏感 → 越"主导"。
    #   ★ 为什么不建议用"把某通道抹平再看概率掉多少"那套：本任务的 5 个通道
    #     彼此高度冗余，抹掉一路剩下的 4 路照样判得对，跌幅会算成 0.000
    #     （真踩过：5 路全 0，白折腾）。梯度是逐样本、逐通道的**局部敏感度**，
    #     不存在这个退化。
    Xh = tf.constant(Xho_n, dtype=tf.float32)
    with tf.GradientTape() as tape:
        tape.watch(Xh)
        p = m(Xh, training=False)
        ptrue = tf.gather_nd(p, tf.stack(
            [tf.range(len(X_ho)), tf.cast(y_ho, tf.int32)], axis=1))
    grad = tape.gradient(ptrue, Xh).numpy()          # (N, 5)
    imp = np.abs(grad).mean(axis=0)
    imp = imp / (imp.sum() + 1e-12)
    rank = np.argsort(-imp)

    log("\n【题2 演示】对留出的 %d 条「后续数据」逐条判定：" % len(X_ho))
    log("  #  真实  判定  置信度  结果   主导特征（本样本 |梯度| 前二）")
    rows_txt, ok_n = [], 0
    for k in range(len(X_ho)):
        pr = p_ho[k]
        cls = int(pr.argmax())
        conf = float(pr[cls])
        hit = (cls == y_ho[k])
        ok_n += hit
        top2 = [FEAT_CN[use[ci]] for ci in np.argsort(-np.abs(grad[k]))[:2]]
        rows_txt.append("  %-2d %-5s %-5s %-6s %-5s  %s"
                        % (k + 1, "板%d" % (y_ho[k] + 1), "板%d" % (cls + 1),
                           "%.1f%%" % (conf * 100), "✔" if hit else "✘", "、".join(top2)))
        log(rows_txt[-1])
    log("  %d 条里判对 %d 条（%.0f%%）"
        % (len(X_ho), ok_n, 100.0 * ok_n / max(1, len(X_ho))))
    log("\n  通道重要性（全样本平均 |梯度|，已归一化）—— 这就是题面「哪个传感器」的答案：")
    for ci in rank:
        log("    %-6s %.3f   %s" % (FEAT_CN[use[ci]], imp[ci],
                                    "█" * max(1, int(imp[ci] * 40))))

    # ---- 可选：分布偏移实测（用**另一个工况**采的数据评估本模型）----
    #   ★ 为什么需要这一步：模型只在"采集时那个摆放"下成立。如果演示时两块板
    #     贴得更近（比如 CAN 线只有 10cm），环境差异会消失 → 准确率可能崩。
    #     与其在答辩现场赌，不如**先采 30 条演示工况的数据当测试集**，实测一次。
    if args.eval_csv:
        # ★ 变量名特意不叫 ep —— 上面 ep 是"训练轮数"（报告里要用），别覆盖它
        eval_path = (args.eval_csv if os.path.isabs(args.eval_csv)
                     else os.path.join(ROOT, args.eval_csv))
        if not os.path.exists(eval_path):
            log("\n（--eval-csv 找不到 %s，跳过）" % eval_path)
        else:
            Xe, ye, _ = load_csv(eval_path)
            Xn_e = (Xe - mean) / std
            pe = m.predict(Xn_e, verbose=0)
            yhe = pe.argmax(axis=1)
            ace = float((yhe == ye).mean())
            cme = np.zeros((2, 2), dtype=int)
            for t, q in zip(ye, yhe):
                cme[int(t)][int(q)] += 1
            log("\n【分布偏移实测】拿 %s 评估本模型（%d 条，未参与训练）"
                % (os.path.relpath(eval_path, ROOT).replace("\\", "/"), len(ye)))
            log("  准确率 %.4f" % ace)
            log("             预测板1  预测板2")
            log("    真实板1   %5d    %5d" % (cme[0][0], cme[0][1]))
            log("    真实板2   %5d    %5d" % (cme[1][0], cme[1][1]))
            if ace >= 0.9:
                log("  ✔ 在**这个工况**下也成立 —— 可以直接照这个摆法演示。")
            else:
                log("  ✘ 明显低于训练环境 → **采集工况 ≠ 演示工况**，别硬上。两条路：")
                log("     · 把两块板按**采集时**的相对摆法/朝向摆（差 10cm 也能摆出同样的相对关系）；")
                log("     · 或者把两份 CSV 合成一份再训（让模型见过两种工况）。")

    # ---- 7) 落盘：模型 + 固件头文件 + 报告 ----
    ai_dir = os.path.join(ROOT, "ai", "model")
    os.makedirs(ai_dir, exist_ok=True)
    fw_ai = os.path.join(ROOT, "firmware", "USER", "AI")
    os.makedirs(fw_ai, exist_ok=True)
    rpt = os.path.join(ROOT, "ai", "report")
    os.makedirs(rpt, exist_ok=True)

    h5 = os.path.join(ai_dir, "model.h5")
    try:
        m.save(h5)
        log("\n模型已存：%s（X-CUBE-AI 首选导入格式）" % os.path.relpath(h5, ROOT))
    except Exception as e:
        log("\n（.h5 保存失败：%s）" % e)
    try:
        m.save(os.path.join(ai_dir, "model.keras"))
    except Exception:
        pass

    # ONNX 兜底（手工建图：Gemm→Relu→Gemm→Relu→Gemm→Softmax，权重直接搬过来）
    try:
        import onnx
        from onnx import TensorProto, helper, numpy_helper
        W = [np.asarray(w, dtype="float32") for w in m.get_weights()]
        nodes, inits, prev = [], [], "input"
        names = ["W1", "B1", "W2", "B2", "W3", "B3"]
        for i in range(0, len(W), 2):
            k = i // 2
            inits.append(numpy_helper.from_array(W[i], names[i]))
            inits.append(numpy_helper.from_array(W[i + 1], names[i + 1]))
            g = "gemm%d" % (k + 1)
            nodes.append(helper.make_node("Gemm", [prev, names[i], names[i + 1]], [g]))
            prev = g
            if i + 2 < len(W):
                r = "relu%d" % (k + 1)
                nodes.append(helper.make_node("Relu", [g], [r]))
                prev = r
        nodes.append(helper.make_node("Softmax", [prev], ["output"], axis=1))
        graph = helper.make_graph(
            nodes, "board_clf",
            [helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", len(use)])],
            [helper.make_tensor_value_info("output", TensorProto.FLOAT, ["N", 2])],
            inits)
        mod = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        mod.ir_version = 9
        onnx.save(mod, os.path.join(ai_dir, "model.onnx"))
        log("ONNX 已存：ai/model/model.onnx（★ 转 C 用这个 —— stedgeai 认不了 Keras 3 的 .h5）")
    except Exception as e:
        log("（ONNX 导出跳过：%s）" % e)

    # ai_norm.h —— 板上归一化必须用同一套 mean/std
    norm_h = os.path.join(fw_ai, "ai_norm.h")
    with open(norm_h, "w", encoding="utf-8") as f:
        f.write("/*========================================================================\n"
                " *  ai_norm.h —— 由 tools/train_nn.py 自动生成，**不要手改**\n"
                " *\n"
                " *  板上推理前必须先做和训练时**完全一样**的 z-score：\n"
                " *      x_norm[i] = (x_raw[i] - AI_MEAN[i]) / AI_STD[i]\n"
                " *  忘了这一步 / 用错参数，网线会给出完全离谱的判定 —— 这是\n"
                " *  「PC 训练 + 板上推理」最容易错的一环，所以参数随模型一起导出。\n"
                " *\n"
                " *  ★ AI_NORM_N = 本模型**实际用了几个通道**（--features 决定），\n"
                " *    固件 Ai_Infer() 只归一化 / 喂前 AI_NORM_N 个。\n"
                " *  特征顺序：%s\n"
                " *  样本数：训练集 %d 条\n"
                " *  数据源：%s\n"
                " *  生成时间：%s\n"
                " *========================================================================*/\n"
                "#ifndef __AI_NORM_H\n#define __AI_NORM_H\n\n"
                "#define AI_NORM_N   %dU\n\n"
                "static const float AI_MEAN[AI_NORM_N] = {%s};\n"
                "static const float AI_STD[AI_NORM_N]  = {%s};\n\n"
                "#endif\n"
                % (" / ".join(FEAT_CN[f] for f in use),
                   len(ytr),
                   os.path.relpath(path, ROOT).replace("\\", "/"),
                   datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                   len(use),
                   ", ".join("%.8ff" % v for v in mean),
                   ", ".join("%.8ff" % v for v in std)))
    log("固件头文件：%s" % os.path.relpath(norm_h, ROOT))

    # ai_test10.h —— 板端演示用的 10 条（原始值 + 真实标签）
    t10 = os.path.join(fw_ai, "ai_test10.h")
    with open(t10, "w", encoding="utf-8") as f:
        f.write("/*========================================================================\n"
                " *  ai_test10.h —— 由 tools/train_nn.py 自动生成，**不要手改**\n"
                " *\n"
                " *  题2 要求的「任取 10 条后续采集的记录」：这 10 条**没有参与训练**\n"
                " *  （是每板时间上最后 %d 条），留给板端逐条做推理演示。\n"
                " *  值都是**原始值**，固件里要先用 ai_norm.h 归一化再喂给 ai_run()。\n"
                " *\n"
                " *  列：temp(℃) humi(%%RH) light(0..4095) eco2(ppm) tvoc(ppb)\n"
                " *  数据源：%s\n"
                " *  生成时间：%s\n"
                " *========================================================================*/\n"
                "#ifndef __AI_TEST10_H\n#define __AI_TEST10_H\n\n"
                "#define AI_TEST10_N   %dU\n\n"
                "static const float AI_TEST10[AI_TEST10_N][5] = {\n"
                % (ho,
                   os.path.relpath(path, ROOT).replace("\\", "/"),
                   datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                   len(X_ho)))
        for k in range(len(X_ho)):
            f.write("    {%8.1ff, %8.1ff, %8.1ff, %8.1ff, %8.1ff},   /* #%d 真值=板%d */\n"
                    % (X_ho_all[k][0], X_ho_all[k][1], X_ho_all[k][2],
                       X_ho_all[k][3], X_ho_all[k][4],
                       k + 1, y_ho[k] + 1))
        f.write("};\n\nstatic const unsigned char AI_TEST10_TRUE[AI_TEST10_N] = {%s};\n\n"
                "#endif\n" % ", ".join(str(int(v) + 1) for v in y_ho))
    log("板端演示样本：%s（10 条，未参与训练）" % os.path.relpath(t10, ROOT))

    # 报告
    md = os.path.join(rpt, "train_report.md")
    with open(md, "w", encoding="utf-8") as f:
        f.write("# 实验七 题2 · 神经网络训练报告\n\n")
        f.write("> 由 `tools/train_nn.py` 自动生成；数据源 `%s`\n\n"
                % os.path.relpath(path, ROOT).replace("\\", "/"))
        f.write("## 1. 数据\n\n")
        _alm = sum(1 for r in raw if str(r.get("status", "OK")).upper() == "ALM")
        f.write("- 总样本 %d 条（板1 %d / 板2 %d）\n"
                % (len(y), int((y == 0).sum()), int((y == 1).sum())))
        if _alm == 0:
            f.write("- 状态位：全部为 `OK`（采集时未发生超温报警）\n")
        else:
            f.write("- 状态位：其中 `ALM`（超温报警）**%d 条**、`OK` %d 条。\n"
                    "  ★ `ALM` 只是板端的状态标记（温度越过报警阈值），**5 个传感器数值本身正常**，\n"
                    "    且训练只使用数值特征 ⇒ 本报告**未剔除** `ALM` 行；\n"
                    "    原因与规避见 `docs/`「采集前串口敲 a 35」。\n"
                    % (_alm, len(y) - _alm))
        f.write("- 训练 %d / 验证 %d / **留出演示 %d**（每板最后 %d 条，不参与训练）\n\n"
                % (len(ytr), len(yva), len(X_ho), ho))
        f.write("## 2. 模型\n\n")
        f.write("| 层 | 输出形状 | 参数量 |\n|---|---|---|\n")
        for l in m.layers:
            if hasattr(l, "kernel"):
                f.write("| %s | %s | %d |\n"
                        % (l.name, str(tuple(l.output.shape[1:])),
                           int(np.prod(l.kernel.shape)) + int(np.prod(l.bias.shape))))
        f.write("\n结构：%s；参数量 **%d**（≈%.2f KB float32）\n\n"
                % (" → ".join(["5"] + [str(h) for h in hid] + ["2(softmax)"]),
                   n_par, n_par * 4 / 1024.0))
        f.write("归一化（**板上必须一致**）：\n\n")
        f.write("| 通道 | mean | std |\n|---|---|---|\n")
        for i, k in enumerate(use):
            f.write("| %s | %.4f | %.4f |\n" % (FEAT_CN[k], mean[i], std[i]))
        f.write("\n## 3. 训练结果\n\n")
        f.write("- 训练轮数 %d（早停）\n- **训练准确率 %.4f**\n- **验证准确率 %.4f**（混淆矩阵重算 %.4f）\n\n"
                % (ep, tr_acc, va_acc, acc_va))
        f.write("验证集混淆矩阵（行=真实，列=预测）：\n\n")
        f.write("|  | 预测板1 | 预测板2 |\n|---|---|---|\n")
        f.write("| **真实板1** | %d | %d |\n| **真实板2** | %d | %d |\n\n"
                % (cm[0][0], cm[0][1], cm[1][0], cm[1][1]))
        # ★ 通道重要性要给「两种视角」：
        #   ① 单通道可分性（AUC / 标准化均值差）—— 稳定、可复现、直接回答题面「哪个传感器」
        #   ② 模型梯度显著性 |∂p/∂x| —— 表示模型实际在靠哪些通道，但**输出饱和时会整体变小**，
        #      排序在不同训练轮次之间会跳（实测同数据两次训练，温度该值 0.06 ↔ 0.43）⇒ 只作参考。
        def _auc(x, yy):
            """Mann-Whitney U / 秩和公式算单通道 AUC（取 max(a,1-a)，避免方向歧义）。"""
            ranks = np.empty(len(x), dtype="float64")
            ranks[np.argsort(x)] = np.arange(1, len(x) + 1)
            npos = int((yy == 1).sum())
            nneg = len(yy) - npos
            if npos == 0 or nneg == 0:
                return 0.5
            a = (ranks[yy == 1].sum() - npos * (npos + 1) / 2.0) / (npos * nneg)
            return max(a, 1.0 - a)

        f.write("## 4. 通道重要性（题面「哪个传感器」的答案）\n\n")
        f.write("**① 单通道可分性**（结论以此为准）—— 只看这一个通道能不能分开两板：\n\n")
        f.write("| 通道 | 板1 均值 | 板2 均值 | 相对差 | 单通道 AUC |\n|---|---|---|---|---|\n")
        _sep = []
        for i, k in enumerate(use):
            x = X[:, i]
            m1 = float(x[y == 0].mean()) if (y == 0).any() else 0.0
            m2 = float(x[y == 1].mean()) if (y == 1).any() else 0.0
            s1 = float(x[y == 0].std(ddof=1)) if (y == 0).sum() > 1 else 0.0
            s2 = float(x[y == 1].std(ddof=1)) if (y == 1).sum() > 1 else 0.0
            sb = (s1 + s2) / 2.0
            _sep.append((abs(m2 - m1) / sb if sb > 0 else 0.0, _auc(x, y), k, m1, m2))
        for sig, a, k, m1, m2 in sorted(_sep, key=lambda t: -t[0]):
            f.write("| %s | %.2f | %.2f | **%.2fσ** | %.3f |\n"
                    % (FEAT_CN[k], m1, m2, sig, a))
        f.write("\n② 模型梯度显著性（全样本平均 |∂p/∂x|，已归一化）—— 模型**实际**在靠哪些通道：\n\n")
        for ci in rank:
            f.write("- %s：%.3f\n" % (FEAT_CN[use[ci]], imp[ci]))
        f.write("\n> ★ ② 的局限：模型输出一旦饱和（置信度≈100%），|∂p/∂x| 会整体变小，\n"
                "> **排序在不同训练轮次之间并不稳定**，所以它只能说明「模型把权重放在哪」,\n"
                "> **不能**用来回答「哪个传感器最能区分两块板」—— 那个问题看 ① 的 AUC / 相对差。\n")
        f.write("\n## 5. 题2 演示：10 条「后续数据」的逐条判定\n\n")
        f.write("```\n" + "\n".join(rows_txt) + "\n```\n\n")
        f.write("判对 %d/%d。这两列也是板端 `ai_test10.h` 里固化的那 10 条。\n" % (ok_n, len(X_ho)))
    log("训练报告：%s" % os.path.relpath(md, ROOT))

    # 混淆矩阵图
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
        plt.rcParams["axes.unicode_minus"] = False
        fig, ax = plt.subplots(figsize=(3.6, 3.2), dpi=130)
        im = ax.imshow(cm, cmap="Blues")
        ax.set_xticks([0, 1]); ax.set_xticklabels(["预测板1", "预测板2"])
        ax.set_yticks([0, 1]); ax.set_yticklabels(["真实板1", "真实板2"])
        for i in range(2):
            for j in range(2):
                ax.text(j, i, cm[i][j], ha="center", va="center",
                        color="white" if cm[i][j] > cm.max() / 2 else "black", fontsize=14)
        ax.set_title("验证集混淆矩阵")
        fig.tight_layout()
        f3 = os.path.join(rpt, "confusion_matrix.png")
        fig.savefig(f3); plt.close(fig)
        log("混淆矩阵图：%s" % os.path.relpath(f3, ROOT))
    except Exception as e:
        log("（出图跳过：%s）" % e)

    # 训练曲线
    try:
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(5.2, 3.2), dpi=130)
        ax.plot(hist.history["accuracy"], label="训练准确率")
        ax.plot(hist.history["val_accuracy"], label="验证准确率")
        ax.set_xlabel("轮次"); ax.set_ylabel("准确率"); ax.grid(alpha=0.25); ax.legend()
        ax.set_title("训练曲线")
        fig.tight_layout()
        f4 = os.path.join(rpt, "train_curve.png")
        fig.savefig(f4); plt.close(fig)
        log("训练曲线：%s" % os.path.relpath(f4, ROOT))
    except Exception:
        pass

    log("\n下一步（题2 收尾）—— ★ 下面 ① 不能省，否则板上跑的还是上一版网络：")
    log("  ① python tools/gen_ai_code.py        （把 ai/model/model.onnx 重转成 network*.c）")
    log("     只跑本脚本 ⇒ 板上 =「新的归一化参数 + 旧的网络权重」= 看着有数其实全错。")
    log("  ② python tools/make_project.py firmware")
    log("  ③ cd firmware/MDK-ARM 编译 board1/board2（都要 0E0W）→ 重新烧写两个 hex")
    log("  ④ 板上验：串口 `n`（跑 ai_test10.h 的 10 条）/ 网页 AI 列 / CAN 0x110 闭环（G5/G6/G8）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
