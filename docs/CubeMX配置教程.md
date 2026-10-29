# 实验六 · STM32CubeMX 配置教程（照做即可）

> 目标：在**不破坏既有配置**的前提下，用 CubeMX 给工程加入 **CAN1**（实验六唯一的"总线"外设）。
> 环境：**STM32CubeMX 6.18.1 + STM32Cube FW_F4 V1.28.3**（与本工程 `.ioc` 一致）。
> 工程文件：`Lab6/firmware/firmware.ioc`

---

## ★ 先记住三条铁律

1. **CAN1 不能选 PD0/PD1**（那两个脚被 FSMC 占用，是 **LCD 数据线**，抢了会花屏）。
   CubeMX 会自动把 CAN1 分到 **PA11/PA12** —— **保持默认即可**（这类 F407 教学板的板载 TJA1050 收发器就接在 PA11/PA12）。
2. **生成代码后，必须再跑一次 `tools/make_project.py`。**
   否则 Keil 工程里的 **USER 分组会被 CubeMX 清空**，我们手写的驱动全部不入编译。
3. **生成代码后，必须把 startup 的 `Stack_Size` 改回 `0x1000`。**
   CubeMX 每次都把它写回默认 `0x400`。

---

## 0. 前置

- 确认工作区干净（本工程已提交基线 `ee1bc8f`），必要时先 `git add -A && git commit`。
- 关闭 Keil（避免文件占用）。

## 1. 打开工程

1. 启动 CubeMX → **File → Load Project** → 选 `Lab6/firmware/firmware.ioc`
2. 若弹"Migrate to newer version / 迁移"提示 → 选 **Migrate**（本工程已是 V1.28.3，通常不弹）

## 2. 配置 CAN1（本次唯一要新增的外设）

### 2.1 启用

左侧 **Pinout & Configuration → Connectivity → CAN1**，在 **Mode** 区勾选 **Activated**。

### 2.2 引脚 —— 保持 CubeMX 自动分配的 PA11/PA12（**无需改**）

CubeMX 激活 CAN1 后会自动分配引脚。因为本工程 **PD0/PD1 已被 FSMC(LCD) 占用**，
CubeMX 会避开它们、把 CAN1 放到 **PA11(CAN1_RX) / PA12(CAN1_TX)** —— **这个分配就是对的，保持原样**。

| 引脚 | 功能 | 模式 |
|---|---|---|
| **PA11** | CAN1_RX | Alternate Function Push Pull，Very High Speed |
| **PA12** | CAN1_TX | Alternate Function Push Pull，Very High Speed |

> 依据：F407 的 CAN1 可复用为 **PA11/PA12** 或 PB8/PB9 或 PD0/PD1。
> 常见 F407 教学板（启明欣欣 / 正点原子探索者）的**板载 TJA1050 收发器就是接 PA11/PA12**，
> 原理图上还经过**跳线帽**把这俩脚引到收发器（见 §10）。**PD0/PD1 不能选**（LCD 数据线）。

**什么情况下才需要改成 PB8/PB9？**
只有当你板子的 CAN 收发器实际接的是 **PB8/PB9** 时（少数板）：
1. 右侧芯片图上左键点 **PB8** → 选 **CAN1_RX**；再点 **PB9** → 选 **CAN1_TX**
2. PA11/PA12 上的 CAN1 标记会自动让出

> 一般**不需要**这一步。用 PA11/PA12 还是 PB8/PB9，**只取决于你板子的硬件接线** —— 软件侧两个都支持。

### 2.3 参数（Parameter Settings）

**Bit Timings**（波特率页签）：

| 项 | 值 |
|---|---|
| Prescaler (for Time Quantum) | **6** |
| Time Quanta in Bit Segment 1 | **10 Times** |
| Time Quanta in Bit Segment 2 | **3 Times** |

→ 右侧 **Baud Rate 应显示 `500000 bit/s`**，采样点 ≈ 78.6%。

> 算法：APB1 = 42MHz，42MHz ÷ 6 = 7MHz；每比特 = 1(Sync) + 10(Seg1) + 3(Seg2) = 14 tq；7MHz ÷ 14 = **500 kHz**。

**其它参数：**

| 项 | 值 |
|---|---|
| Operating Mode | **Normal** |
| Time Triggered Communication Mode | Disable |
| Automatic Bus-Off Management | **Enable** |
| Automatic Wake-Up Mode | Disable |
| Automatic Retransmission | **Enable** |
| Receive Fifo Locked Mode | Disable |
| Transmit Fifo Priority | Disable |

### 2.4 NVIC（中断）

**不要勾选** "CAN1 RX0 interrupt"。

本工程的 CAN 驱动用 **轮询**（`CanTask` 每 10ms 取 FIFO0）—— 1Hz 的数据率下绰绰有余，还能避免动中断回调。若勾了中断却不写回调，会走进默认 `Handler` 死循环。

## 3. 确认既有外设（不要动）

| 外设 | 现状 | 说明 |
|---|---|---|
| **I2C1** | PB6/PB7，100kHz | **SHT30(0x44)** 挂这条，无需改 |
| **I2C2** | **已加**（§12） | **SGP30(0x58)** 挂这条（**PF0/PF1**） |
| FSMC | 保持 | LCD（ILI9341） |
| ADC1 | PC0 | 光敏 AO |
| UART4 | PA0/PA1 | ESP-01S |
| USART1 | PA9/PA10 | 调试串口 |
| FreeRTOS / SYS | 保持 | RTOS + 时基 |

## 4. 时钟（Clock Configuration 页）

确认 **APB1 = 42MHz**（CAN 挂在 APB1）。若主频仍是 168MHz，此处应已自动正确。

## 5. 工程设置（Project Manager）

**Project 页：**

- Toolchain / IDE = **MDK-ARM V5**

**Code Generator 页：**

- ✔ 勾选 **"Generate peripheral initialization as a pair of .c/.h files per peripheral"**
  （这样 CAN 会生成到独立的 `Core/Src/can.c`、`Core/Inc/can.h`）
- ✘ **不要**勾 "Delete previously generated files when not re-generated"
- 其余保持默认

## 6. 生成代码

点右上 **GENERATE CODE**。

生成后应出现：

- `Core/Src/can.c` + `Core/Inc/can.h`（含 `MX_CAN1_Init()` / `hcan1`）
- `main.c` 的初始化序列里多出 `MX_CAN1_Init();`
- `Core/Inc/stm32f4xx_hal_conf.h` 里 **`HAL_CAN_MODULE_ENABLED` 被自动打开**
- `Drivers/STM32F4xx_HAL_Driver/` 下多出 `stm32f4xx_hal_can.c` / `.h`

## 7. 生成后必做两步（★ 否则编不过）

### 7.1 跑 make_project.py（恢复 USER 分组 + 重建三个 Target）

```bash
cd E:/Develop/SignalsAndSystems/Lab6
python tools/make_project.py firmware
```

应输出：

```
  Target board1  BOARD_SEL=1  挂入 13 个源文件
  Target board2  BOARD_SEL=2  挂入 13 个源文件
  Target board3  BOARD_SEL=3  挂入 13 个源文件
```

（CubeMX 生成的工程只有 1 个 Target、且 USER 组被清空；此脚本把它们补回来。）

### 7.2 改回 startup 栈大小

文件 `firmware/MDK-ARM/startup_stm32f407xx.s`，找到：

```asm
Stack_Size      EQU     0x400
```

改成：

```asm
Stack_Size      EQU     0x1000
```

## 8. 编译验证

Keil 里依次编译 **board1 / board2 / board3** 三个 Target，确认 **0 Error 0 Warning**。

命令行方式（供参考）：

```bash
cd firmware/MDK-ARM
"/d/Keil_v5/UV4/UV4.exe" -b firmware.uvprojx -t board1 -o build_b1.log
"/d/Keil_v5/UV4/UV4.exe" -b firmware.uvprojx -t board2 -o build_b2.log
"/d/Keil_v5/UV4/UV4.exe" -b firmware.uvprojx -t board3 -o build_b3.log
grep -E "Error\(s\)|Warning\(s\)|Program Size" build_b1.log build_b2.log build_b3.log
```

## 9. 生成后检查清单

- [ ] `Core/Src/can.c` 存在，`main.c` 里有 `MX_CAN1_Init();`
- [ ] `Core/Inc/can.h` 里有 `extern CAN_HandleTypeDef hcan1;`
- [ ] `hal_conf.h` 里 `#define HAL_CAN_MODULE_ENABLED` 已启用（不是注释）
- [ ] `Drivers/.../stm32f4xx_hal_can.c` 与 `.h` 存在
- [ ] `can.c` / `gpio.c` 里 **PA11/PA12 配为 AF9**（不是 PD0/PD1）
- [ ] 已跑 `make_project.py`，Keil 的 **USER 组里有 `can_link.c`、`sgp30.c`**
- [ ] `startup_stm32f407xx.s` 的 `Stack_Size = 0x1000`

---

## 10. 硬件接线（CAN 总线）★ 已按《启明欣欣 407 V6.1 原理图》核对

```
板1 ─┐
板2 ─┼── CANH（板上标 1_H）────── CANH ─┐
板3 ─┘    CANL（板上标 1_L）────── CANL ─┘
   （三块板必须共地 GND）
   ★ 不必外接 120Ω —— 每块板已板载 R35 = 120Ω
```

- **收发器板载**：本板自带 **U9 = TJA1050**（VCC5 供电），**不用外接模块**
- MCU 侧（原理图证实）：`PA11 = CAN1_RX → U9 的 R(脚4)`；`PA12 = CAN1_TX → U9 的 D(脚1)`
- **共地**是必须的，否则差分信号没有共模参考，通信不稳
- 波特率三块板必须一致（都是 500kbps）
- **终端电阻不要再加**：每板自带 120Ω，三板并联 ≈40Ω。
  短母线 + 500k 一般够用；若时断时续，第一嫌疑在这里（不是"再加电阻"能修的）

### ★★ 启明欣欣 407 V6.1：必须短接 P21（最关键的一步）

PA11/PA12 在板上**同时**接到 USB 座，用 **P21**（2×3 排针）二选一：

```
        P21
USB_DM1  1 ── 2  USB_DP1     ← 接到 USB 座 J9
USB_D-   3 ── 4  USB_D+      ← 就是 MCU 的 PA11 / PA12
CAN1_RX  5 ── 6  CAN1_TX     ← 接到 TJA1050 (U9)
```

| 用途 | P21 短接 |
|---|---|
| **用 CAN1（本实验）** | ★ **3–5 和 4–6**（两个短路帽） |
| 用 USB | 1–3 和 2–4 |

> **症状对照**：P21 没跳到 CAN 档 = **程序全对、CAN 总线上毫无波形**。
> 上板前先看一眼 P21 上两个短路帽插在哪一排。

（更多核对结论见 `docs/硬件核对-原理图.md`。）

---

## 11. 常见问题速查

| 现象 | 原因 / 处理 |
|---|---|
| CubeMX 报引脚冲突 | PD0/PD1 被 FSMC 占了 —— CubeMX 会自动改用 **PA11/PA12**，保持默认即可 |
| Keil 编译找不到 `sgp30.c` / `can_link.c` | 忘了跑 `make_project.py`（USER 组被 CubeMX 清空） |
| 程序跑飞 / HardFault | `Stack_Size` 被 CubeMX 改回 `0x400` 了 —— 改回 `0x1000` |
| CAN 完全收不到 | ①板载收发器跳线帽是否短接；②120Ω 终端电阻；③**共地**；④收发器供电；⑤三块板波特率都是 500k；⑥`PA11→TXD`/`PA12→RXD` 别接反 |
| 一进 CAN 就卡死 | NVIC 里勾了 RX0 中断但没写回调 —— 去 CubeMX 取消勾选 |
| `HAL_CAN_MODULE_ENABLED` 仍是注释 | Code Generator 里没勾"生成独立 .c/.h"，或生成失败，重来 |

---

## 12. 启用 I2C2（SGP30 走独立总线）

> 目的：**SHT30 与 SGP30 各走一条 I2C** —— 每根针只对一个设备，**不必在总线针上分叉**。
> SHT30 仍用 I2C1（PB6/PB7）；**SGP30 改到 I2C2（本板 CubeMX 分配在 PF0/PF1）**。

**① Connectivity → I2C2 → Mode**：选 **I2C**（★ 本工程已加好，此节留作参考）

**② 引脚分配**

| 引脚 | 功能 |
|---|---|
| **PF1** | **I2C2_SCL** |
| **PF0** | **I2C2_SDA** |

> ★ 若 CubeMX 默认给了别的位置（如 PB10/PB11）也**都行** —— 只要这两个脚在板上有引出即可。
> 本板实测：**PF0/PF1 是空脚且在 P7 排针上引出**（原理图确认无其它网络）。

**③ Parameter Settings：与 I2C1 保持完全一致**
（Speed Mode = **Standard**、Clock Speed = **100000**、其余默认）
—— 两条总线参数不一致会出现"一条能通、一条不通"。
★ I2C2 也挂在 **APB1**，**时钟树不用改**。

**④ 生成代码**：`Core/Src/i2c.c` 里会多出 `hi2c2` + `MX_I2C2_Init()`，
`main.c` 的调用序列会自动补上。

**⑤ 固件侧只改一行** —— `USER/SGP30/sgp30.h`：

```c
#define SGP30_I2C_HANDLE   hi2c2      /* 原为 hi2c1 */
```

**⑥ 重新编译**，确认 **0 Error 0 Warning**。

> ★ 以后若想回到"共用 I2C1"，把这行改回 `hi2c1` 就行，**不用动 CubeMX**。

---

## 附：本次配置"只做加法"的边界

- **新增 2 个外设**：**CAN1**（§2）和 **I2C2**（§12，给 SGP30 独立总线）。
- I2C1 / FSMC / ADC1 / UART4 / USART1 / FreeRTOS 全部保持原样。
- SGP30 挂 **I2C2（PF0/PF1）**，地址 `0x58`；SHT30 挂 **I2C1（PB6/PB7）**，地址 `0x44`。
  两条总线互不影响。
- 三块板烧同一工程的三个 Target（board1/2/3），硬件完全一样。
