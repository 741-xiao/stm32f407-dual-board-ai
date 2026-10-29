#ifndef __SHT30_H
#define __SHT30_H

#include "common.h"

/* SHT30 默认地址 0x44（HAL 使用 8 位地址 0x88）。 */
#define SHT30_I2C_ADDRESS       (0x44U << 1)

/* 单次测量命令（高重复度，开启时钟拉伸） */
#define SHT30_CMD_MEAS_HIGH_NOSTRETCH   0x2400U
#define SHT30_CMD_MEAS_HIGH_STRETCH     0x2C06U
#define SHT30_CMD_SOFT_RESET            0x30A2U

/* I2C 单次传输超时（★ 原 100ms 太长：传感器没接时每次干等 100ms 纯空转）*/
#define SHT30_I2C_TIMEOUT_MS            20U

/* 软复位传感器，退出时传感器已就绪。
 * 注意：I2C1 外设与 PB6/PB7 引脚已由 CubeMX 的 MX_I2C1_Init() 配置，
 *       本函数不再重复初始化，只负责对传感器下达命令。 */
HAL_StatusTypeDef SHT30_Init(void);

/* 读取一次温湿度原始值（各 16 位，已通过 CRC-8 校验）。 */
HAL_StatusTypeDef SHT30_ReadRaw(uint16_t *temperature, uint16_t *humidity);

/* 读一次并换算成“放大 10 倍”的整数：
 *   temp10 单位 0.1℃（例如 256 表示 25.6℃）
 *   humi10 单位 0.1%RH（例如 523 表示 52.3%RH） */
HAL_StatusTypeDef SHT30_ReadTempHumi10(s32 *temp10, s32 *humi10);

#endif
