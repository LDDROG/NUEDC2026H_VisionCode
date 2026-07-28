#ifndef AS5600_H
#define AS5600_H

#include <stdint.h>

#include "bsp_iic.h"

#define AS5600_I2C_ADDR         0x36    // 7位设备地址

// 状态寄存器
#define AS5600_REG_STATUS       0x0B    // 状态
#define AS5600_REG_RAW_ANGLE    0x0C    // 原始角度 (12位, 推荐用这个)
#define AS5600_REG_ANGLE        0x0E    // 角度 (经过配置的零点和范围)
#define AS5600_REG_AGC          0x1A    // 自动增益控制
#define AS5600_REG_MAGNITUDE    0x1B    // 磁场强度

// 配置寄存器
#define AS5600_REG_ZPOS_H       0x01    // 零点位置高字节
#define AS5600_REG_CONF_H       0x07    // 配置高字节

/**
 * @brief 初始化AS5600驱动
 * @param conf IIC初始化配置
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t AS5600_Init(IIC_Init_Config_s *conf);

/**
 * @brief 设置AS5600底层IIC工作模式
 * @param mode IIC工作模式
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t AS5600_SetIICMode(IIC_Work_Mode_e mode);

/**
 * @brief 按指定IIC模式读取原始角度
 * @param mode IIC工作模式
 * @param raw_angle 输出原始角度(12bit)
 * @return uint8_t 0:成功, 1:失败
 */
uint8_t AS5600_ReadRawAngleByMode(IIC_Work_Mode_e mode, uint16_t *raw_angle);

/**
 * @brief 读取AS5600原始角度(默认走当前配置)
 * @return uint16_t 原始角度值(0~4095)
 */
uint16_t AS5600_ReadRawAngle(void);

/**
 * @brief 读取AS5600角度寄存器
 * @return uint16_t 角度值(0~4095)
 */
uint16_t AS5600_ReadAngle(void);

/**
 * @brief 读取角度(度)
 * @return float 角度值(0~360)
 */
float AS5600_ReadAngleDegree(void);

/**
 * @brief 读取诊断信息
 * @param agc 输出AGC
 * @param magnitude 输出磁场强度
 */
void AS5600_ReadDiagnostics(uint8_t *agc, uint16_t *magnitude);

/**
 * @brief 以当前位置设置零点
 */
void AS5600_SetZeroPosition(void);

/**
 * @brief 读取配置寄存器
 * @return uint16_t 配置值
 */
uint16_t AS5600_ReadConfig(void);

/**
 * @brief 根据相邻两次角度估算速度
 * @return float 角速度(度每秒)
 */
float AS5600_GetVelocity(void);

#endif