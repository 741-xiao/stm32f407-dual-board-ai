/*========================================================================
 *  ai_norm.h —— 由 tools/train_nn.py 自动生成，**不要手改**
 *
 *  板上推理前必须先做和训练时**完全一样**的 z-score：
 *      x_norm[i] = (x_raw[i] - AI_MEAN[i]) / AI_STD[i]
 *  忘了这一步 / 用错参数，网线会给出完全离谱的判定 —— 这是
 *  「PC 训练 + 板上推理」最容易错的一环，所以参数随模型一起导出。
 *
 *  ★ AI_NORM_N = 本模型**实际用了几个通道**（--features 决定），
 *    固件 Ai_Infer() 只归一化 / 喂前 AI_NORM_N 个。
 *  特征顺序：温度 / 湿度
 *  样本数：训练集 109 条
 *  数据源：ai/dataset/lab7.csv
 *  生成时间：2026-10-24 16:53:25
 *========================================================================*/
#ifndef __AI_NORM_H
#define __AI_NORM_H

#define AI_NORM_N   2U

static const float AI_MEAN[AI_NORM_N] = {24.13579559f, 65.79636383f};
static const float AI_STD[AI_NORM_N]  = {0.19472474f, 3.33318782f};

#endif
