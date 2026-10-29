#include "sgp30.h"

/*==============================================================================
 *  SGP30 空气质量传感器驱动
 *  说明：I2C 的 GPIO 与时钟由 CubeMX 的 MX_I2Cx_Init() 完成（本工程：SGP30 挂
 *        I2C2 = PF0(SDA)/PF1(SCL)；SHT30 挂 I2C1 = PB6/PB7）。这里只保留"和传感器对话"的部分。
 *        挂哪条 I2C 由 sgp30.h 的 SGP30_I2C_HANDLE 决定（现为 hi2c2，与 SHT30 分开）。
 *============================================================================*/

extern I2C_HandleTypeDef hi2c1;     /* 由 Core/Src/i2c.c 提供（SHT30 用）*/
extern I2C_HandleTypeDef hi2c2;     /* ★ 本工程 SGP30 用这条（CubeMX 生成）*/

/* CRC-8 校验：多项式 0x31，初值 0xFF（与 SHT30 同口径） */
static uint8_t SGP30_Crc8(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0xFFU;
    uint8_t bit;

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

/* 发一条 2 字节命令 */
static HAL_StatusTypeDef SGP30_SendCmd(uint16_t cmd)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(cmd >> 8);
    buf[1] = (uint8_t)(cmd & 0xFFU);
    return HAL_I2C_Master_Transmit(&SGP30_I2C_HANDLE, SGP30_I2C_ADDRESS, buf, 2U, SGP30_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef SGP30_Init(void)
{
    HAL_StatusTypeDef status;

    /* 下发 IAQ init；手册要求随后等约 10ms 让传感器就绪 */
    status = SGP30_SendCmd(SGP30_CMD_IAQ_INIT);
    delay_ms(10);
    return status;
}

/*------------------------------------------------------------------------------
 *  ★ 两段式（非阻塞）：任务里先 Start，vTaskDelay 让出 CPU，再 Fetch。
 *------------------------------------------------------------------------------*/
HAL_StatusTypeDef SGP30_StartMeasure(void)
{
    return SGP30_SendCmd(SGP30_CMD_MEASURE_IAQ);
}

HAL_StatusTypeDef SGP30_FetchIAQ(u16 *eco2, u16 *tvoc)
{
    uint8_t data[6];
    HAL_StatusTypeDef status;

    status = HAL_I2C_Master_Receive(&SGP30_I2C_HANDLE, SGP30_I2C_ADDRESS, data, 6U, SGP30_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    /* 6 字节：eCO2(2) + CRC(1) + TVOC(2) + CRC(1) */
    if ((SGP30_Crc8(data, 2U) != data[2]) ||
        (SGP30_Crc8(&data[3], 2U) != data[5]))
    {
        return HAL_ERROR;
    }

    *eco2 = (u16)(((u16)data[0] << 8) | data[1]);
    *tvoc = (u16)(((u16)data[3] << 8) | data[4]);
    return HAL_OK;
}

/* 一步到位（内部忙等）—— 只在启动期/非 RTOS 场合用 */
HAL_StatusTypeDef SGP30_ReadIAQ(u16 *eco2, u16 *tvoc)
{
    HAL_StatusTypeDef status;

    status = SGP30_StartMeasure();
    if (status != HAL_OK)
    {
        return status;
    }
    delay_ms(SGP30_MEASURE_WAIT_MS);
    return SGP30_FetchIAQ(eco2, tvoc);
}

HAL_StatusTypeDef SGP30_SetHumidity(u16 rh_abs)
{
    uint8_t buf[5];
    uint8_t crc;

    buf[0] = (uint8_t)(SGP30_CMD_SET_HUMIDITY >> 8);
    buf[1] = (uint8_t)(SGP30_CMD_SET_HUMIDITY & 0xFFU);
    buf[2] = (uint8_t)(rh_abs >> 8);
    buf[3] = (uint8_t)(rh_abs & 0xFFU);
    crc    = SGP30_Crc8(&buf[2], 2U);
    buf[4] = crc;

    return HAL_I2C_Master_Transmit(&SGP30_I2C_HANDLE, SGP30_I2C_ADDRESS, buf, 5U, SGP30_I2C_TIMEOUT_MS);
}
