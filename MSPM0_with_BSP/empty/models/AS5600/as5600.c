#include "as5600.h"

#include "bsp_time.h"

#define AS5600_TRANSFER_WAIT_LIMIT (2000000U)

static IICInstance *s_as5600_iic = NULL;
static TimeInstance *s_as5600_timer = NULL;
static float s_angle_prev_deg = 0.0f;
static float s_angle_curr_deg = 0.0f;
static float s_delta_sec = 0.0f;

/**
 * @brief 等待一次非阻塞IIC传输完成
 * @return uint8_t 0:成功, 1:超时
 */
static uint8_t as5600_wait_transfer_done(void)
{
    uint32_t timeout = AS5600_TRANSFER_WAIT_LIMIT;

    if (s_as5600_iic == NULL) {
        return 1U;
    }

    while ((s_as5600_iic->transfer_busy) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout == 0U) ? 1U : 0U;
}

/**
 * @brief 将两个字节拼接为12位角度
 * @param hi 高字节
 * @param lo 低字节
 * @return uint16_t 12位角度值
 */
static uint16_t as5600_decode_angle12(uint8_t hi, uint8_t lo)
{
    uint16_t angle = ((uint16_t) hi << 8) | (uint16_t) lo;
    return (uint16_t) (angle & 0x0FFFU);
}

/**
 * @brief 按指定模式读取AS5600的双字节寄存器
 * @param mode IIC工作模式
 * @param reg_addr 寄存器地址
 * @param out_buf 输出缓冲区(2字节)
 * @return uint8_t 0:成功, 1:失败
 */
static uint8_t as5600_read_reg16_by_mode(IIC_Work_Mode_e mode, uint8_t reg_addr,
    uint8_t out_buf[2])
{
    if ((s_as5600_iic == NULL) || (out_buf == NULL)) {
        return 1U;
    }

    IICSetMode(s_as5600_iic, mode);

    if (mode == IIC_BLOCK_MODE) {
        IICAccessMem(s_as5600_iic, reg_addr, out_buf, 2U, IIC_READ_MEM, 1U);
        return 0U;
    }

    IICTransmit(s_as5600_iic, &reg_addr, 1U, IIC_SEQ_HOLDON);
    if (as5600_wait_transfer_done() != 0U) {
        return 1U;
    }

    IICReceive(s_as5600_iic, out_buf, 2U, IIC_SEQ_RELEASE);
    if (as5600_wait_transfer_done() != 0U) {
        return 1U;
    }

    return 0U;
}

/**
 * @brief 初始化AS5600驱动
 * @param conf IIC初始化配置
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t AS5600_Init(IIC_Init_Config_s *conf)
{
    Time_Init_Config_s timer_conf = {
        .id = "timer_for_as5600",
    };

    if (conf == NULL) {
        return 1U;
    }

    s_as5600_iic = IICRegister(conf);
    if (s_as5600_iic == NULL) {
        return 1U;
    }

    s_as5600_timer = TimeRegister(&timer_conf);
    if (s_as5600_timer == NULL) {
        return 1U;
    }

    TimeMark(s_as5600_timer);
    s_angle_prev_deg = 0.0f;
    s_angle_curr_deg = 0.0f;
    s_delta_sec = 0.0f;

    return 0U;
}

/**
 * @brief 设置AS5600底层IIC工作模式
 * @param mode IIC工作模式
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t AS5600_SetIICMode(IIC_Work_Mode_e mode)
{
    if (s_as5600_iic == NULL) {
        return 1U;
    }

    IICSetMode(s_as5600_iic, mode);
    return 0U;
}

/**
 * @brief 按指定IIC模式读取原始角度
 * @param mode IIC工作模式
 * @param raw_angle 输出原始角度(12bit)
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t AS5600_ReadRawAngleByMode(IIC_Work_Mode_e mode, uint16_t *raw_angle)
{
    uint8_t buf[2] = {0U, 0U};

    if (raw_angle == NULL) {
        return 1U;
    }

    if (as5600_read_reg16_by_mode(mode, AS5600_REG_RAW_ANGLE, buf) != 0U) {
        return 1U;
    }

    *raw_angle = as5600_decode_angle12(buf[0], buf[1]);
    return 0U;
}

/**
 * @brief 读取AS5600原始角度(默认阻塞)
 * @return uint16_t 原始角度值(0~4095)
 */
uint16_t AS5600_ReadRawAngle(void)
{
    uint16_t raw = 0U;
    (void) AS5600_ReadRawAngleByMode(IIC_BLOCK_MODE, &raw);
    return raw;
}

/**
 * @brief 读取AS5600角度寄存器(默认阻塞)
 * @return uint16_t 角度值(0~4095)
 */
uint16_t AS5600_ReadAngle(void)
{
    uint8_t buf[2] = {0U, 0U};

    if (as5600_read_reg16_by_mode(IIC_BLOCK_MODE, AS5600_REG_ANGLE, buf) != 0U) {
        return 0U;
    }

    return as5600_decode_angle12(buf[0], buf[1]);
}

/**
 * @brief 读取角度(度)
 * @return float 角度值(0~360)
 */
float AS5600_ReadAngleDegree(void)
{
    uint16_t raw = AS5600_ReadRawAngle();

    if (s_as5600_timer != NULL) {
        s_delta_sec = ((float) TimeGetElapsedUs(s_as5600_timer)) * 1e-6f;
        TimeMark(s_as5600_timer);
    } else {
        s_delta_sec = 0.0f;
    }

    s_angle_prev_deg = s_angle_curr_deg;
    s_angle_curr_deg = ((float) raw * 360.0f) / 4096.0f;

    return s_angle_curr_deg;
}

/**
 * @brief 根据相邻两次角度估算速度
 * @return float 角速度(度每秒)
 */
float AS5600_GetVelocity(void)
{
    if (s_delta_sec <= 0.0f) {
        return 0.0f;
    }
    return (s_angle_curr_deg - s_angle_prev_deg) / s_delta_sec;
}

/**
 * @brief 读取磁场强度和AGC信息（阻塞）
 * @param agc 输出AGC
 * @param magnitude 输出磁场强度
 */
void AS5600_ReadDiagnostics(uint8_t *agc, uint16_t *magnitude)
{
    uint8_t buf[2] = {0U, 0U};

    if ((s_as5600_iic == NULL) || (agc == NULL) || (magnitude == NULL)) {
        return;
    }

    IICSetMode(s_as5600_iic, IIC_BLOCK_MODE);
    IICAccessMem(s_as5600_iic, AS5600_REG_AGC, agc, 1U, IIC_READ_MEM, 1U);
    IICAccessMem(s_as5600_iic, AS5600_REG_MAGNITUDE, buf, 2U, IIC_READ_MEM, 1U);
    *magnitude = ((uint16_t) buf[0] << 8) | (uint16_t) buf[1];
}

/**
 * @brief 以当前位置设置零点（阻塞）
 */
void AS5600_SetZeroPosition(void)
{
    uint16_t current = AS5600_ReadRawAngle();
    uint8_t zpos[2];

    if (s_as5600_iic == NULL) {
        return;
    }

    zpos[0] = (uint8_t) ((current >> 8) & 0x0FU);
    zpos[1] = (uint8_t) (current & 0xFFU);

    IICSetMode(s_as5600_iic, IIC_BLOCK_MODE);
    IICAccessMem(s_as5600_iic, AS5600_REG_ZPOS_H, zpos, 2U, IIC_WRITE_MEM, 1U);
}

/**
 * @brief 读取AS5600配置（阻塞）
 * @return uint16_t 配置值
 */
uint16_t AS5600_ReadConfig(void)
{
    uint8_t buf[2] = {0U, 0U};

    if (s_as5600_iic == NULL) {
        return 0U;
    }

    IICSetMode(s_as5600_iic, IIC_BLOCK_MODE);
    IICAccessMem(s_as5600_iic, AS5600_REG_CONF_H, buf, 2U, IIC_READ_MEM, 1U);

    return ((uint16_t) buf[0] << 8) | (uint16_t) buf[1];
}