#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""★ 题2 的前置关卡：**可分性检验**（先判断"能不能训"，再谈训练）。

为什么必须先做：两块板如果摆在同一个位置、同样的环境里，同型号传感器的读数分布
几乎完全重合 —— 这时候神经网络只能学到接近随机的映射，训完才发现就白费一天。
本脚本用轻量分类器做 5 折交叉验证 + 逐通道统计 + PCA 投影，**先给出结论**：

    准确率 ≥ 80%  → 可分，可以进入训练（tools/train_nn.py）
    准确率 < 80%  → 先调整两板摆放（制造稳定的环境差异）再重采

依赖：numpy / scikit-learn / matplotlib
    pip install numpy scikit-learn matplotlib
若本机用的是受管环境的 3.13 解释器，可直接：
    C:\\Users\\<你>\\.workbuddy\\binaries\\python\\envs\\default\\Scripts\\python.exe tools\\check_separability.py ...

用法：
    python tools/check_separability.py                     # 默认读 ai/dataset/lab7.csv
    python tools/check_separability.py ai/dataset/run2.csv
    python tools/check_separability.py --folds 10          # 折数（默认 5）
"""

import argparse
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FEATS = ["temp", "humi", "light", "eco2", "tvoc"]
FEAT_CN = {"temp": "温度", "humi": "湿度", "light": "光敏",
           "eco2": "CO2", "tvoc": "TVOC"}


def load_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    X, y, meta = [], [], []
    for r in rows:
        try:
            X.append([float(r[k]) for k in FEATS])
        except (ValueError, KeyError):
            continue
        y.append(int(r["board"]))          # 1 / 2
        meta.append(r)
    return X, y, meta


def main():
    ap = argparse.ArgumentParser(description="题2 前置：两板数据可分性检验")
    ap.add_argument("csv", nargs="?", default=os.path.join("ai", "dataset", "lab7.csv"))
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--thr", type=float, default=0.80, help="判定「可分」的准确率门槛")
    args = ap.parse_args()

    path = args.csv if os.path.isabs(args.csv) else os.path.join(ROOT, args.csv)
    if not os.path.exists(path):
        print("✘ 找不到 %s\n  先跑：python tools/export_dataset.py" % path)
        return 2

    try:
        import numpy as np
        from sklearn.linear_model import LogisticRegression
        from sklearn.ensemble import RandomForestClassifier
        from sklearn.model_selection import StratifiedKFold, cross_val_score
        from sklearn.pipeline import make_pipeline
        from sklearn.preprocessing import StandardScaler
        from sklearn.metrics import roc_auc_score, confusion_matrix
    except ImportError as e:
        print("✘ 缺依赖：%s\n  pip install numpy scikit-learn matplotlib" % e)
        return 2

    X, y, meta = load_csv(path)
    n1 = sum(1 for v in y if v == 1)
    n2 = sum(1 for v in y if v == 2)
    print("=" * 74)
    print("  可分性检验  %s" % os.path.relpath(path, ROOT))
    print("  板1 %d 条 / 板2 %d 条，共 %d 条；特征 %s" % (n1, n2, len(X), "/".join(FEATS)))
    print("=" * 74)
    if min(n1, n2) < 20:
        print("✘ 某一块板样本太少（<20），先回去多采点。")
        return 1

    Xa = np.asarray(X, dtype=float)
    ya = np.asarray([0 if v == 1 else 1 for v in y])   # 0=板1 1=板2
    lines = []

    def say(s=""):
        print(s)
        lines.append(s)

    # ---- 1) 逐通道统计：两板差多少 ----
    say("\n【1】逐通道统计（板1 vs 板2）—— 差异越大越好判")
    say("  %-6s %-18s %-18s %-10s %s" % ("通道", "板1 均值±标准差", "板2 均值±标准差",
                                         "相对差", "单通道AUC"))
    auc_single = {}
    for i, k in enumerate(FEATS):
        a = Xa[ya == 0, i]
        b = Xa[ya == 1, i]
        try:
            auc = roc_auc_score(ya, Xa[:, i])
        except Exception:
            auc = 0.5
        # ★ 取 max(auc, 1-auc)：单通道 AUC 只看"分不分得开"，不关心方向
        #   （板1 湿度 58.7 > 板2 55.8 → 原始 AUC=0.000 看着像"完全没用"，
        #    其实是完美反序；不这么处理会误导人。）
        auc = max(auc, 1.0 - auc)
        auc_single[k] = auc
        # 相对差 = |Δ均值| / 两板标准差均值（>1 说明两簇基本分开）
        sd = (a.std() + b.std()) / 2.0 or 1e-9
        rel = abs(a.mean() - b.mean()) / sd
        say("  %-6s %-18s %-18s %-10s %.3f%s"
            % (FEAT_CN[k], "%.1f ± %.1f" % (a.mean(), a.std()),
               "%.1f ± %.1f" % (b.mean(), b.std()),
               "%.2fσ" % rel, auc, "  ← 这个通道最有区分度" if rel > 1.0 else ""))

    # ---- 2) 单通道单独能不能判 ----
    say("\n【2】只用单个通道做 5 折交叉验证（看信号主要落在哪个传感器上）")
    cv = StratifiedKFold(n_splits=args.folds, shuffle=True, random_state=42)
    single = {}
    for i, k in enumerate(FEATS):
        clf = make_pipeline(StandardScaler(), LogisticRegression(max_iter=1000))
        acc = cross_val_score(clf, Xa[:, [i]], ya, cv=cv, scoring="accuracy").mean()
        single[k] = acc
        say("  %-6s 准确率 %.3f" % (FEAT_CN[k], acc))

    # ---- 3) 五通道合起来 ----
    say("\n【3】5 个通道合起来，%d 折交叉验证" % args.folds)
    models = [
        ("逻辑回归", make_pipeline(StandardScaler(), LogisticRegression(max_iter=2000))),
        ("随机森林", RandomForestClassifier(n_estimators=300, random_state=42)),
    ]
    best_name, best_acc = "", 0.0
    for name, m in models:
        s = cross_val_score(m, Xa, ya, cv=cv, scoring="accuracy")
        say("  %-8s 准确率 %.3f ± %.3f   （各折 %s）"
            % (name, s.mean(), s.std(), " ".join("%.2f" % v for v in s)))
        if s.mean() > best_acc:
            best_name, best_acc = name, s.mean()

    # 混淆矩阵（用全部数据训练 + 交叉验证预测拼起来，看两类错法）
    try:
        from sklearn.model_selection import cross_val_predict
        clf = make_pipeline(StandardScaler(), LogisticRegression(max_iter=2000))
        pred = cross_val_predict(clf, Xa, ya, cv=cv)
        cm = confusion_matrix(ya, pred)
        say("\n  %s 的混淆矩阵（行=真实，列=预测；0=板1 1=板2）" % "逻辑回归")
        say("           预测板1  预测板2")
        say("    真实板1   %5d    %5d" % (cm[0][0], cm[0][1]))
        say("    真实板2   %5d    %5d" % (cm[1][0], cm[1][1]))
    except Exception:
        pass

    # ---- 4) 结论 ----
    say("\n【4】结论")
    ok = best_acc >= args.thr
    if ok:
        say("  ✔ 最好模型 %s 准确率 %.3f ≥ %.2f —— **可分**，可以进入训练。" % (best_name, best_acc, args.thr))
        say("    最强通道：%s" % "、".join(
            FEAT_CN[k] for k, _ in sorted(single.items(), key=lambda kv: -kv[1])[:2]))
    else:
        say("  ✘ 最好模型 %s 准确率 %.3f < %.2f —— **不够可分**，先别训。" % (best_name, best_acc, args.thr))
        say("    按下面顺序调整（一次只改一件事，再重采一遍）：")
        say("      ① 让两板的光照条件不同（一块朝窗/被台灯照，一块背光）—— 光敏差异最立竿见影；")
        say("      ② 让两板附近空气不同（一块靠近人的呼吸方向/热源）—— CO₂ 与温度会有稳定差；")
        say("      ③ 换个位置（别并排贴在一起），并保持这个摆放**别动**；")
        say("      ④ 若还是不行：把两板的采温点垫高/贴不同材质，制造稳定的热惯性差异。")
        say("    注意这**不是造假**：题2 要的就是「从数据反推来源」的能力，")
        say("    两板本来就该处于各自真实的、可区分的工况。")

    # ---- 5) 出图（文档/答辩要用）----
    rpt = os.path.join(ROOT, "ai", "report")
    os.makedirs(rpt, exist_ok=True)
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
        plt.rcParams["axes.unicode_minus"] = False

        # PCA 散点
        from sklearn.decomposition import PCA
        p = PCA(n_components=2).fit(Xa)
        Z = p.transform(Xa)
        fig, ax = plt.subplots(figsize=(5.6, 4.4), dpi=130)
        for b, c, nm in ((0, "#378ADD", "板1"), (1, "#EF9F27", "板2")):
            ax.scatter(Z[ya == b, 0], Z[ya == b, 1], s=14, alpha=0.7, c=c, label=nm)
        ax.set_title("PCA 二维投影（两簇分得开 = 可分）")
        ax.set_xlabel("主成分1（%.0f%% 方差）" % (p.explained_variance_ratio_[0] * 100))
        ax.set_ylabel("主成分2（%.0f%% 方差）" % (p.explained_variance_ratio_[1] * 100))
        ax.legend(); ax.grid(alpha=0.25)
        fig.tight_layout()
        f1 = os.path.join(rpt, "sep_pca.png")
        fig.savefig(f1); plt.close(fig)

        # 逐通道直方图
        fig, axes = plt.subplots(1, 5, figsize=(15, 3.0), dpi=130)
        for i, k in enumerate(FEATS):
            ax = axes[i]
            ax.hist(Xa[ya == 0, i], bins=16, alpha=0.65, color="#378ADD", label="板1")
            ax.hist(Xa[ya == 1, i], bins=16, alpha=0.65, color="#EF9F27", label="板2")
            ax.set_title("%s（AUC %.2f）" % (FEAT_CN[k], auc_single[k]), fontsize=10)
            ax.grid(alpha=0.2)
        axes[0].legend(fontsize=8)
        fig.tight_layout()
        f2 = os.path.join(rpt, "sep_hist.png")
        fig.savefig(f2); plt.close(fig)
        say("\n  图已存：%s / %s" % (os.path.relpath(f1, ROOT), os.path.relpath(f2, ROOT)))
    except Exception as e:
        say("\n  （出图跳过：%s）" % e)

    # ---- 6) 落盘报告 ----
    md = os.path.join(rpt, "separability.md")
    with open(md, "w", encoding="utf-8") as f:
        f.write("# 实验七 题2 · 两板数据可分性检验报告\n\n")
        f.write("> 由 `tools/check_separability.py` 自动生成，数据源：`%s`\n\n"
                % os.path.relpath(path, ROOT).replace("\\", "/"))
        f.write("```\n" + "\n".join(lines) + "\n```\n")
    print("\n报告已存：%s" % os.path.relpath(md, ROOT))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
