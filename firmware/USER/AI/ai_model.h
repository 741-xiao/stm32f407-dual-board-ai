/*==============================================================================
 *  ai_model.h  ---  题2 神经网络推理的**薄封装**（把"模型"和"应用"隔开）
 *
 *  为什么要有这一层：
 *    1) X-CUBE-AI 生成的代码，函数名/文件名取决于 CubeMX 里给网络起的名字
 *       （默认 `network`），而且集成前后代码要能**照样编译**。把调用点收在这
 *       一个文件里，集成就只改这一处，app 层完全不用动。
 *    2) 归一化（z-score）是"PC 训练 / 板上推理"最容易错的一环，也收在这里：
 *       参数来自 tools/train_nn.py 生成的 ai_norm.h，与训练时**同一套**。
 *
 *  集成步骤（CubeMX 生成代码之后）：
 *    ① 把生成的 `X-CUBE-AI/App/` 下的 .c/.h 和 `Middlewares/ST/AI/` 拷进
 *       `firmware/USER/AI/`，并把 .c 加进 tools/make_project.py 的 USER_FILES
 *    ② 把下面 AI_ENABLED 改成 1，并按生成的 API 名填好 Ai_Infer() 里的调用
 *    ③ python tools/make_project.py firmware  → 编译
 *
 *  接口约定（app 层只认这三个）：
 *    Ai_Ready()  —— 1 = 已集成（可以推理）
 *    Ai_Infer()  —— 输入本板 5 项原始值，输出判定（1=板1 / 2=板2）+ 置信度 0..100
 *    Ai_Info()   —— 一行文本，给串口/屏上显示"模型是什么"
 *============================================================================*/
#ifndef __AI_MODEL_H
#define __AI_MODEL_H

#include "common.h"

/* ★ 集成 X-CUBE-AI 生成代码后改成 1（改完记得重新编译两个 Target）
   —— 2026-10-12：已集成 network.c/network_data.c + NetworkRuntime1020_CM4_Keil.lib */
#define AI_ENABLED   1

/* 1 = 模型可用；0 = 还没集成（此时 Ai_Infer 直接返回 0，app 层显示 "--"）*/
u8 Ai_Ready(void);

/* 推理一次。
 *   temp10 : 温度 ×10（有符号，可负）
 *   humi10 : 湿度 ×10
 *   light  : 光敏原始值 0..4095
 *   eco2   : SGP30 eCO2 (ppm)
 *   tvoc   : SGP30 TVOC (ppb)
 *   *cls   : 输出判定 —— 1 = 板1，2 = 板2
 *   *conf  : 输出置信度 0..100
 * 返回 1 = 推理成功；0 = 不可用/失败（调用方别用 *cls）
 * ★ 单位换算与归一化都在**本函数内部**做，调用方直接喂 g 里的字段即可。 */
u8 Ai_Infer(s32 temp10, s32 humi10, u32 light, u32 eco2, u32 tvoc,
            u8 *cls, u8 *conf);

/* 一行说明（模型名 / 参数 / 是否已集成），给串口 `s`、`n` 命令打印 */
const char *Ai_Info(void);

#endif
