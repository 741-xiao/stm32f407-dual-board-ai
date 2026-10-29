#include "sht30.h"

/*==============================================================================
 *  SHT30 温湿度传感器（I2C1 / PB6=SCL, PB7=SDA）
 *  说明：I2C1 的 GPIO 与时钟配置由 CubeMX 的 MX_I2C1_Init() 完成，
 *        这里只保留“和传感器对话”的部分，避免与 Core/Src/i2c.c 重复定义 hi2c1。
 *============================================================================*/

extern I2C_HandleTypeDef hi2c1;     /* 由 Core/Src/i2c.c 提供 */

/* CRC-8 校验：多项式 0x31，初值 0xFF */
static uint8_t SHT30_Crc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc;
    uint8_t bit;

    crc = 0xFFU;
    while (length-- != 0U)
    {
        crc ^= *data++;
        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1) ^ 0x31U);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* 软件复位，等待约 2ms 让传感器重新就绪 */
HAL_StatusTypeDef SHT30_Init(void)
{
    uint8_t command[2];
    HAL_StatusTypeDef status;

    command[0] = (uint8_t)(SHT30_CMD_SOFT_RESET >> 8);
    command[1] = (uint8_t)(SHT30_CMD_SOFT_RESET & 0xFFU);

    status = HAL_I2C_Master_Transmit(&hi2c1, SHT30_I2C_ADDRESS, command, 2U, SHT30_I2C_TIMEOUT_MS);
    delay_ms(2);
    return status;
}

HAL_StatusTypeDef SHT30_ReadRaw(uint16_t *temperature, uint16_t *humidity)
{
    uint8_t command[2];
    uint8_t data[6];
    HAL_StatusTypeDef status;

    command[0] = (uint8_t)(SHT30_CMD_MEAS_HIGH_STRETCH >> 8);
    command[1] = (uint8_t)(SHT30_CMD_MEAS_HIGH_STRETCH & 0xFFU);

    status = HAL_I2C_Master_Transmit(&hi2c1, SHT30_I2C_ADDRESS, command, 2U, SHT30_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_I2C_Master_Receive(&hi2c1, SHT30_I2C_ADDRESS, data, 6U, SHT30_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    if ((SHT30_Crc8(data, 2U) != data[2]) ||
        (SHT30_Crc8(&data[3], 2U) != data[5]))
    {
        return HAL_ERROR;
    }

    *temperature = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    *humidity    = (uint16_t)(((uint16_t)data[3] << 8) | data[4]);
    return HAL_OK;
}

HAL_StatusTypeDef SHT30_ReadTempHumi10(s32 *temp10, s32 *humi10)
{
    uint16_t raw_temperature;
    uint16_t raw_humidity;
    HAL_StatusTypeDef status;

    status = SHT30_ReadRaw(&raw_temperature, &raw_humidity);
    if (status != HAL_OK)
    {
        return status;
    }

    /* 数据手册：T(℃) = -45 + 175 * S_T / 65535
     *           RH(%) = 100 * S_RH / 65535
     * 先在整数域放大 10 倍，避免浮点运算。 */
    *temp10 = ((s32)175 * (s32)raw_temperature * 10) / 65535 - 450;
    *humi10 = ((s32)1000 * (s32)raw_humidity) / 65535;
    return HAL_OK;
}
