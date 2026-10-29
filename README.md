# STM32F407 双板 CAN 互联、WiFi 上报与边缘 AI

两块 STM32F407 通过 CAN 同步采集状态，并各自通过 ESP8266 将传感器数据上传到 PC。上位机负责入库、网页展示和模型训练，X-CUBE-AI 模型部署在板端用于判断数据来源。

实验作者：郑鑫，学号 202440700111  
整理与开源发布：肖肃，`xiao6665678@qq.com`

## 功能

- 两块板通过 CAN1 以 500 kbit/s 互联，任意一块板的 KEY0/KEY1 可同步开始或停止采集。
- 板端采集温度、湿度、光照、eCO2 和 TVOC，并在 LCD 与呼吸灯上显示状态。
- 两块 ESP8266 分别通过 WiFi/TCP 向 PC 上传本板数据。
- Python 上位机接收数据、写入 MySQL，并提供记录列表和图表网页。
- 数据集导出、可分性检查、TensorFlow 训练、ONNX 导出和 X-CUBE-AI C 代码生成。
- 两个 Keil Target 共用源码，通过 `BOARD_SEL` 生成板 1/板 2 固件。

## 系统结构

```text
        PC（移动热点网关通常为 192.168.137.1）
        ├── 上位机 TCP:8080，接收两块板数据 -> MySQL stm32_lab.lab7_data
        ├── 记录页面 http://127.0.0.1:8000/
        └── 图表页面 http://127.0.0.1:8000/chart
               ▲ WiFi                         ▲ WiFi
          ┌────┴────┐        CAN1         ┌────┴────┐
          │ 板 1    │─────500 kbit/s─────│ 板 2    │
          │ 主控+AI │                     │ 从机+AI │
          └─────────┘                     └─────────┘
```

板 1 额外将本板数据通过 CAN 广播给板 2；两块板各自上传本板数据到 PC。

## 目录

| 路径 | 内容 |
|---|---|
| `firmware/` | Keil 工程，包含 `board1` 和 `board2` 两个 Target |
| `firmware/USER/AI/` | X-CUBE-AI 生成代码、归一化参数和演示样本 |
| `pc/` | 上位机 `upper_computer.py`、建表脚本和配置示例 |
| `tools/` | 建库、导出数据、训练、模型转换、同步检查和假板模拟 |
| `ai/` | 模型、数据集与训练报告；大型数据和模型默认不入库 |
| `docs/` | 需求、方案、硬件核对、接线和答辩材料 |

## 运行依赖

- Python 3.11 或更高版本
- MySQL 8.x
- Keil MDK / ARM Compiler 5
- STM32Cube.AI 的 `stedgeai` 命令行工具

建议使用独立虚拟环境：

```bat
python -m venv .venv
.venv\Scripts\python.exe -m pip install tensorflow scikit-learn matplotlib numpy onnx pymysql
set PY=%~dp0.venv\Scripts\python.exe
```

TensorFlow 对 Python 版本有要求；若安装失败，请先调整 Python 版本或按报错安装兼容版本。

## 一次性配置

1. 在 `firmware/USER/App/app_id.h` 中填写 2.4 GHz WiFi 名称和密码。仓库中只保留占位符。
2. 在 PC 上开启移动热点，查询并确认电脑局域网 IP。
3. 根据热点环境检查 `firmware/USER/WIFI/esp01s.h` 中的 PC 地址和板端静态地址。
4. 创建 MySQL 数据库并生成本地配置：

```bat
python tools\init_db.py
copy pc\db_local.example.py pc\db_local.py
```

编辑 `pc\db_local.py` 填入本机 MySQL 口令。该文件已被 `.gitignore` 排除，不会提交。

## 运行上位机

```bat
python pc\upper_computer.py
```

上位机监听 TCP `8080`，网页监听 TCP `8000`。没有硬件时，可在另一个终端启动假板：

```bat
python tools\pc_sim_boards.py --ai
```

浏览器入口：

- 记录列表：<http://127.0.0.1:8000/>
- 图表页面：<http://127.0.0.1:8000/chart>

## 硬件准备

| 事项 | 要求 |
|---|---|
| CAN 接线 | CAN_H 对 CAN_H、CAN_L 对 CAN_L，两板必须共地 |
| CAN 终端 | 按所用开发板原理图确认板载 120 Ω 电阻，避免重复并联 |
| P21 跳线 | 按开发板说明选择 PA11/PA12 的 CAN 功能，取消 USB 占用 |
| WiFi | 两块 ESP8266 连接同一个 2.4 GHz 热点，并与 PC 网络互通 |
| 上电顺序 | 先板 2、再板 1 |
| 烧录文件 | `firmware/MDK-ARM/board1/board1.hex`、`firmware/MDK-ARM/board2/board2.hex` |

不同版本实验板的 USB/CAN 跳线和终端电阻布局可能不同，请以实际原理图为准。

## 编译与烧录

在 Keil 中分别编译 `board1` 和 `board2` Target。也可以使用项目工具构建，输出位置以脚本日志为准。

烧录后按以下顺序验证：

1. 串口或 LCD 确认 WiFi 已连接。
2. 在任意一块板上按 KEY0，两块板应同时进入采集状态。
3. 记录页面持续出现两块板的数据，图表页面可切换查看。
4. 按 KEY1，两块板应同时停止采集。

## 数据与模型

### 采集和导出

```bat
python tools\look_db.py
python tools\export_dataset.py --min-per-board 100
python tools\check_separability.py
```

采集与演示时两块板的摆放应保持一致，避免训练数据和演示数据分布明显不同。

### 训练

```bat
python tools\train_nn.py
```

训练会生成或更新模型和归一化参数。建议先完成可分性检查，再进入板端部署。

### 转换、同步和编译

```bat
python tools\gen_ai_code.py
python tools\make_project.py firmware
python tools\check_sync.py --hex
```

`train_nn.py` 不会自动替换全部 X-CUBE-AI 网络权重。训练后必须重新运行 `gen_ai_code.py`，再让新文件进入 Keil 工程并重新编译。否则可能出现“新归一化参数 + 旧网络权重”，输出有数值但结果错误。

模型转换器对中文路径兼容性较差，建议把工程放在纯英文路径下操作。

一条命令执行完整训练和构建流程：

```bat
%PY% tools\retrain.py --build
```

常用开关：

- `--check`：检查 Python、依赖、Keil、`stedgeai`、MySQL 和数据状态
- `--skip-export`：使用现有 CSV
- `--skip-sep`：跳过分可性检查
- `--min-per-board 120`：提高每块板最少样本数
- `--epochs 400`：指定训练轮数

## 常见问题

| 现象 | 排查 |
|---|---|
| 网页没有收到任何数据 | 检查板端 WiFi 状态、PC IP、防火墙 TCP 8080 |
| CAN 总线没有波形 | 检查 CAN_H/CAN_L、共地、P21 跳线和终端电阻 |
| 上位机显示数据库未启用 | 安装 `pymysql`，创建 `pc/db_local.py`，确认数据库和表存在 |
| 按键无反应 | 按开发板丝印 KEY0..KEY3 对应，不要按成编号 1..4 |
| 重训后板端结果不变 | 重新执行 `gen_ai_code.py`、`make_project.py` 并重新编译烧录 |
| 编译找不到 AI 头文件 | 重新运行 `python tools\make_project.py firmware` |
| 网页代码修改未生效 | 检查旧的 Python 监听进程是否仍占用 8000 端口 |

## 开发信息

- 实验作者：郑鑫（202440700111）
- 整理发布：肖肃（202440700102），`xiao6665678@qq.com`
- 许可证：[MIT](LICENSE)
