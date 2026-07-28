#include "mpu6500.h"

static IICInstance *s_mpu6500_iic = NULL;

#define MPU6500_DMA_WAIT_LIMIT (2000000U)

/**
 * @brief 读取一个8位寄存器
 * @param reg 寄存器地址
 * @param value 输出数据
 * @return uint8_t 0:成功, 1:失败
 */
static uint8_t mpu6500_read_u8(uint8_t reg, uint8_t *value)
{
    if ((s_mpu6500_iic == NULL) || (value == NULL)) {
        return 1U;
    }

    IICAccessMem(s_mpu6500_iic, reg, value, 1U, IIC_READ_MEM, 1U);
    return 0U;
}

/**
 * @brief 写一个8位寄存器
 * @param reg 寄存器地址
 * @param value 写入数据
 * @return uint8_t 0:成功, 1:失败
 */
static uint8_t mpu6500_write_u8(uint8_t reg, uint8_t value)
{
    if (s_mpu6500_iic == NULL) {
        return 1U;
    }

    IICAccessMem(s_mpu6500_iic, reg, &value, 1U, IIC_WRITE_MEM, 1U);
    return 0U;
}

/**
 * @brief 等待一次非阻塞IIC传输完成
 * @return uint8_t 0:成功, 1:超时或实例无效
 */
static uint8_t mpu6500_wait_transfer_done(void)
{
    uint32_t timeout = MPU6500_DMA_WAIT_LIMIT;

    if (s_mpu6500_iic == NULL) {
        return 1U;
    }

    while ((s_mpu6500_iic->transfer_busy) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout > 0U) ? 0U : 1U;
}

/**
 * @brief 使用IIC DMA模式读取寄存器数据
 * @param reg 起始寄存器地址
 * @param data 输出缓冲区
 * @param size 读取长度
 * @return uint8_t 0:成功, 1:失败
 */
static uint8_t mpu6500_read_dma(uint8_t reg, uint8_t *data, uint16_t size)
{
    if ((s_mpu6500_iic == NULL) || (data == NULL) || (size == 0U)) {
        return 1U;
    }

    IICSetMode(s_mpu6500_iic, IIC_DMA_MODE);

    IICTransmit(s_mpu6500_iic, &reg, 1U, IIC_SEQ_HOLDON);
    if (mpu6500_wait_transfer_done() != 0U) {
        return 1U;
    }

    IICReceive(s_mpu6500_iic, data, size, IIC_SEQ_RELEASE);
    if (mpu6500_wait_transfer_done() != 0U) {
        return 1U;
    }

    return 0U;
}

/**
 * @brief 初始化MPU6500驱动并注册IIC实例
 * @param iic_conf IIC初始化配置
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_Init(IIC_Init_Config_s *iic_conf)
{
    if (iic_conf == NULL) {
        return 1U;
    }

    s_mpu6500_iic = IICRegister(iic_conf);
    if (s_mpu6500_iic == NULL) {
        return 1U;
    }

    IICSetMode(s_mpu6500_iic, IIC_BLOCK_MODE);

    // 退出休眠, 使用内部时钟
    if (mpu6500_write_u8(MPU6500_REG_PWR_MGMT_1, 0x00U) != 0U) {
        return 1U;
    }
    // 采样率分频 = 0 -> 1kHz
    if (mpu6500_write_u8(MPU6500_REG_SMPLRT_DIV, 0x00U) != 0U) {
        return 1U;
    }
    // DLPF配置
    if (mpu6500_write_u8(MPU6500_REG_CONFIG, 0x03U) != 0U) {
        return 1U;
    }
    // 陀螺仪量程 +-2000dps
    if (mpu6500_write_u8(MPU6500_REG_GYRO_CONFIG, 0x18U) != 0U) {
        return 1U;
    }
    // 加速度量程 +-8g
    if (mpu6500_write_u8(MPU6500_REG_ACCEL_CONFIG, 0x10U) != 0U) {
        return 1U;
    }

    return 0U;
}

/**
 * @brief 读取WHO_AM_I寄存器
 * @param who_am_i 输出寄存器值
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadWhoAmI(uint8_t *who_am_i)
{
    return mpu6500_read_u8(MPU6500_REG_WHO_AM_I, who_am_i);
}

/**
 * @brief 读取加速度/陀螺仪/温度原始数据
 * @param raw 输出原始数据
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadRawData(MPU6500_RawData_s *raw)
{
    uint8_t buf[14];

    if ((s_mpu6500_iic == NULL) || (raw == NULL)) {
        return 1U;
    }

    IICAccessMem(s_mpu6500_iic, MPU6500_REG_ACCEL_XOUT_H, buf, 14U, IIC_READ_MEM, 1U);

    raw->ax = (int16_t) ((((uint16_t) buf[0]) << 8) | buf[1]);
    raw->ay = (int16_t) ((((uint16_t) buf[2]) << 8) | buf[3]);
    raw->az = (int16_t) ((((uint16_t) buf[4]) << 8) | buf[5]);
    raw->temp = (int16_t) ((((uint16_t) buf[6]) << 8) | buf[7]);
    raw->gx = (int16_t) ((((uint16_t) buf[8]) << 8) | buf[9]);
    raw->gy = (int16_t) ((((uint16_t) buf[10]) << 8) | buf[11]);
    raw->gz = (int16_t) ((((uint16_t) buf[12]) << 8) | buf[13]);

    return 0U;
}

/**
 * @brief 初始化MPU6500驱动(DMA读测试版本)
 * @param iic_conf IIC初始化配置
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_Init_DMA(IIC_Init_Config_s *iic_conf)
{
    if (MPU6500_Init(iic_conf) != 0U) {
        return 1U;
    }

    IICSetMode(s_mpu6500_iic, IIC_DMA_MODE);
    return 0U;
}

/**
 * @brief 使用DMA流程读取WHO_AM_I寄存器
 * @param who_am_i 输出寄存器值
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadWhoAmI_DMA(uint8_t *who_am_i)
{
    return mpu6500_read_dma(MPU6500_REG_WHO_AM_I, who_am_i, 1U);
}

/**
 * @brief 使用DMA流程读取加速度/陀螺仪/温度原始数据
 * @param raw 输出原始数据
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadRawData_DMA(MPU6500_RawData_s *raw)
{
    uint8_t buf[14];

    if (raw == NULL) {
        return 1U;
    }
    if (mpu6500_read_dma(MPU6500_REG_ACCEL_XOUT_H, buf, 14U) != 0U) {
        return 1U;
    }

    raw->ax = (int16_t) ((((uint16_t) buf[0]) << 8) | buf[1]);
    raw->ay = (int16_t) ((((uint16_t) buf[2]) << 8) | buf[3]);
    raw->az = (int16_t) ((((uint16_t) buf[4]) << 8) | buf[5]);
    raw->temp = (int16_t) ((((uint16_t) buf[6]) << 8) | buf[7]);
    raw->gx = (int16_t) ((((uint16_t) buf[8]) << 8) | buf[9]);
    raw->gy = (int16_t) ((((uint16_t) buf[10]) << 8) | buf[11]);
    raw->gz = (int16_t) ((((uint16_t) buf[12]) << 8) | buf[13]);

    return 0U;
}