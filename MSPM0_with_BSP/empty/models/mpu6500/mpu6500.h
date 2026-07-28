#ifndef MPU6500_H
#define MPU6500_H

#include <stdint.h>

#include "bsp_iic.h"

#define MPU6500_I2C_ADDR        0x68U

#define MPU6500_REG_SMPLRT_DIV  0x19U
#define MPU6500_REG_CONFIG      0x1AU
#define MPU6500_REG_GYRO_CONFIG 0x1BU
#define MPU6500_REG_ACCEL_CONFIG 0x1CU
#define MPU6500_REG_ACCEL_XOUT_H 0x3BU
#define MPU6500_REG_TEMP_OUT_H  0x41U
#define MPU6500_REG_GYRO_XOUT_H 0x43U
#define MPU6500_REG_PWR_MGMT_1  0x6BU
#define MPU6500_REG_WHO_AM_I    0x75U

#define MPU6500_WHO_AM_I_VALUE  0x70U

typedef struct
{
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t gx;
    int16_t gy;
    int16_t gz;
    int16_t temp;
} MPU6500_RawData_s;

/**
 * @brief 初始化MPU6500驱动并注册IIC实例
 * @param iic_conf IIC初始化配置
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_Init(IIC_Init_Config_s *iic_conf);

/**
 * @brief 读取WHO_AM_I寄存器
 * @param who_am_i 输出寄存器值
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadWhoAmI(uint8_t *who_am_i);

/**
 * @brief 读取加速度/陀螺仪/温度原始数据
 * @param raw 输出原始数据
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadRawData(MPU6500_RawData_s *raw);

/**
 * @brief 初始化MPU6500驱动(DMA读测试版本)
 * @param iic_conf IIC初始化配置
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_Init_DMA(IIC_Init_Config_s *iic_conf);

/**
 * @brief 使用DMA流程读取WHO_AM_I寄存器
 * @param who_am_i 输出寄存器值
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadWhoAmI_DMA(uint8_t *who_am_i);

/**
 * @brief 使用DMA流程读取加速度/陀螺仪/温度原始数据
 * @param raw 输出原始数据
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t MPU6500_ReadRawData_DMA(MPU6500_RawData_s *raw);

#endif