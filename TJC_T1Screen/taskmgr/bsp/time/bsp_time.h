/**
 * @file bsp_time.h
 * @brief MSPM0 32位定时器时间库，精度1us
 * @version 1.0
 * @date 2024-XX-XX
 */

#ifndef BSP_TIME_H
#define BSP_TIME_H

#include "ti_msp_dl_config.h"
#include "stdint.h"
#include "stdbool.h"

#define TIME_DEVICE_CNT 8  // 最大支持的时间实例数量

/* 时间实例结构体 */
typedef struct time_ins_temp
{
    uint32_t mark_tick;                      // 标定时间tick
    uint32_t mark_offset_us;                 // 标定时的开机时间偏移(us)
    void *id;                                // 实例ID
    bool is_marked;                          // 是否已标定
} TimeInstance;

typedef struct
{
    void *id;                                // 实例ID
} Time_Init_Config_s;

/**
 * @brief 初始化时间库（在main中调用一次）
 * @note  使用TIMA0或TIMB0作为32位定时器，预分频至1us精度
 */
void TimeInit(void);

/**
 * @brief 注册一个时间实例
 * @param config 初始化配置
 * @return TimeInstance* 时间实例指针
 */
TimeInstance *TimeRegister(Time_Init_Config_s *config);

/**
 * @brief 标定时间（记录当前时间为该实例的零点）
 * @param time 时间实例
 */
void TimeMark(TimeInstance *time);

/**
 * @brief 获取开机到现在的微秒数（全局时间）
 * @return uint32_t 微秒数，溢出后自动回绕
 */
uint32_t TimeGetBootUs(void);

/**
 * @brief 获取开机到现在的毫秒数
 * @return uint32_t 毫秒数
 */
uint32_t TimeGetBootMs(void);

/**
 * @brief 获取当前tick值（32位定时器原始值）
 * @return uint32_t 定时器计数值
 */
uint32_t TimeGetTick(void);

/**
 * @brief 获取距离标定时间经过的微秒数
 * @param time 时间实例
 * @return uint32_t 距离标定时间的微秒数
 */
uint32_t TimeGetElapsedUs(TimeInstance *time);

/**
 * @brief 获取距离标定时间经过的毫秒数
 * @param time 时间实例
 * @return uint32_t 距离标定时间的毫秒数
 */
uint32_t TimeGetElapsedMs(TimeInstance *time);

/**
 * @brief 获取当前时间与标定时间的tick差值（原始tick）
 * @param time 时间实例
 * @return uint32_t tick差值
 */
uint32_t TimeGetTickDelta(TimeInstance *time);

/**
 * @brief 延时微秒（阻塞式）
 * @param us 微秒数
 */
void TimeDelayUs(uint32_t us);

/**
 * @brief 延时毫秒（阻塞式）
 * @param ms 毫秒数
 */
void TimeDelayMs(uint32_t ms);

#endif // BSP_TIME_H