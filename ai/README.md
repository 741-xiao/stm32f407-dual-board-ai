# ai/ —— 题2（神经网络）的工作目录

> 这个目录放**数据、模型、报告**。三者都是"能被脚本重新生成的东西"，
> 所以入库策略是 **只提交生成它们的脚本**（`tools/*.py`）+ 最终固件头文件，
> 数据集和模型本体不入库（见根目录 `.gitignore`）。

## 目录结构

| 路径 | 内容 | 入库？ |
|---|---|---|
| `dataset/*.csv` | 数据集快照（`tools/export_dataset.py` 导出；模拟器 `--csv` 也能直接落） | ✘ 不入库 |
| `model/model.h5` | Keras 模型（存档用；★ stedgeai 10.2.1 **认不了 Keras 3 的 .h5**） | ✘ 不入库 |
| `model/model.onnx` | ★ **转 C 唯一可用格式**（已实测与 Keras 数值等价） | ✘ 不入库 |
| `report/separability.md` + `sep_*.png` | 可分性检验报告（5 折 CV + 逐通道 AUC + PCA 散点 + 直方图） | ✓ 入库 |
| `report/train_report.md` + `*.png` | 训练报告（准确率 / 混淆矩阵 / 通道重要性 / 10 条演示判定） | ✓ 入库 |
| `report/stedgeai_*.txt` + `.json` | stedgeai 的 analyze / generate 报告（Flash/RAM/MACC，题3 文档用） | ✓ 入库 |
| `tmp_gen/` | stedgeai 的原始产物（**存档备查**，工程里用的是 `firmware/USER/AI/`） | ✓ 入库 |
| `../firmware/USER/AI/ai_norm.h` | 归一化参数（mean/std），板上推理必须用**同一套** | ✓ 入库 |
| `../firmware/USER/AI/ai_test10.h` | 题2 演示用的 10 条留出样本（原始值 + 真值） | ✓ 入库 |

## 一条龙：四个脚本（按顺序跑）

```bash
# ① 从 MySQL 导数据集（也可用模拟器 --csv 直接落，先调通流程）
python tools/export_dataset.py --min-per-board 100

# ② ★ 前置关卡：能不能训？（准确率 ≥80% 才算可分，否则先调两板摆放再重采）
python tools/check_separability.py

# ③ 训练 + 导出模型 / ai_norm.h / ai_test10.h / model.h5 / model.onnx
python tools/train_nn.py

# ④ ★★ 把 model.onnx 重转成板端 C 代码（覆盖 firmware/USER/AI/network*.c）
python tools/gen_ai_code.py

# ⑤ 自检：固件里的权重是不是比模型旧？（退出码 1 = 落后，要去跑 ④）
python tools/check_sync.py
```

**④ 是独立的一步，不能省**：③ 只更新 `ai_norm.h`/`ai_test10.h`（归一化参数），
`network*.c`（权重）还停在上一版模型 ⇒ 板上 = 新归一化 + 旧权重 = **输出看着有数、其实全错**。
④ 之后还要 `python tools/make_project.py firmware` → 编译两 Target → **重烧两个 hex**。

依赖（numpy / scikit-learn / matplotlib / tensorflow / onnx / pymysql）
**已装在本机受管 venv**，直接用它跑（系统 python 里没有这些包）：
```bat
set PY=C:\Users\<你>\.workbuddy\binaries\python\envs\default\Scripts\python.exe
%PY% tools\train_nn.py
```
★ 报 `did not find executable at ...\versions\3.13.12\python.exe` = 基础解释器链接断了，
修法见 `.workbuddy/memory/CONVENTIONS.md`「环境事实」。

## ★★ 三条纪律

1. **模拟数据只能用来调流程，不能当题2 的最终训练集写进报告。**
   假板模拟器（`tools/pc_sim_boards.py --csv`）造出来的两组数据是"故意可分"的
   （板1 偏暗偏凉 / 板2 偏亮偏暖），准确率必然 100% —— 那只证明**脚本没写错**，
   不证明你的两板环境可分。报告里必须用**真板采的**数据。
   > 生成的 `ai_norm.h` / `ai_test10.h` 头部都带「数据源 + 生成时间」落款，
   > 一眼就能看出这份参数是拿哪份数据训的 —— 别再出现"分不清哪次训的"。
2. **归一化参数必须和训练时一致**：板上用 `ai_norm.h` 的 mean/std 做 z-score，
   PC 侧导出/复算也要用同一套。忘掉这一步会得到完全离谱的判定，
   这是「PC 训练 + 板上推理」最容易错的一环。
3. ★★★ **采集工况必须 = 演示工况**。CAN 线不够长 ⇒ 演示时两板相隔约 **10cm**，
   那就必须**按 10cm 摆着重采一遍**再做可分性检验。分开摆时采、贴一起演示，
   模型靠的"光敏/CO₂ 环境差"会当场消失 ⇒ **翻车**。
   10cm 下真正能用的只有**器件个体差异** ⇒ 把两板的传感器模块**位置/朝向错开**制造真实安装差异。
   跑完 ③ 后用 `python tools/train_nn.py <主数据> --eval-csv <演示摆法的数据>` 实测一遍分布偏移。
