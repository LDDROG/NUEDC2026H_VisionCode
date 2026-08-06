#include "bsp_time.h"
#include "stdlib.h"
#include "string.h"

// 实例管理
static uint8_t time_idx = 0;
static TimeInstance *time_instance[TIME_DEVICE_CNT] = {NULL};

// 定时器句柄 - 假设使用TIMA0作为32位定时器
static GPTIMER_Regs *time_timer = TIMG12;

// 时钟频率配置（假设系统时钟80MHz，分频后1us = 1tick）
#define TIME_TCLK_MHZ       80      // 定时器时钟MHz
#define TIME_PRESCALER      (80-1)  // 80分频 = 1MHz = 1us/tick

/**
 * @brief 初始化时间库，配置32位定时器
 */
void TimeInit(void)
{

    DL_TimerG_setLoadValue(time_timer, 0xFFFFFFFF); // 最大计数值，自由运行
    DL_TimerG_startCounter(time_timer);
}

/**
 * @brief 注册时间实例
 */
TimeInstance *TimeRegister(Time_Init_Config_s *config)
{
    if (time_idx >= TIME_DEVICE_CNT)
        while(1);  // 实例数超限
    
    TimeInstance *time = (TimeInstance *)malloc(sizeof(TimeInstance));
    memset(time, 0, sizeof(TimeInstance));
    
    time->id = config->id;
    time->is_marked = false;
    time->mark_tick = 0;
    time->mark_offset_us = 0;
    
    time_instance[time_idx++] = time;
    return time;
}

/**
 * @brief 获取当前32位定时器tick值
 */
inline uint32_t TimeGetTick(void)
{
    return DL_TimerG_getTimerCount(time_timer);
}

/**
 * @brief 获取开机时间（微秒）
 */
uint32_t TimeGetBootUs(void)
{
    return TimeGetTick();  // 1tick = 1us
}

/**
 * @brief 获取开机时间（毫秒）
 */
uint32_t TimeGetBootMs(void)
{
    return TimeGetTick() / 1000;
}

/**
 * @brief 标定当前时间（设置实例的时间零点）
 */
void TimeMark(TimeInstance *time)
{
    if (time == NULL) return;
    
    time->mark_tick = TimeGetTick();
    time->mark_offset_us = time->mark_tick;  // 记录标定时的开机时间
    time->is_marked = true;
}

/**
 * @brief 获取距离标定时间经过的微秒数
 */
uint32_t TimeGetElapsedUs(TimeInstance *time)
{
    if (time == NULL || !time->is_marked) return 0;
    
    uint32_t current = TimeGetTick();
    return current - time->mark_tick;  // 32位无符号减法自动处理溢出
}

/**
 * @brief 获取距离标定时间经过的毫秒数
 */
uint32_t TimeGetElapsedMs(TimeInstance *time)
{
    return TimeGetElapsedUs(time) / 1000;
}

/**
 * @brief 获取当前与标定时间的tick差值（同ElapsedUs，但语义更清晰）
 */
uint32_t TimeGetTickDelta(TimeInstance *time)
{
    return TimeGetElapsedUs(time);
}

/**
 * @brief 微秒延时（阻塞）
 */
void TimeDelayUs(uint32_t us)
{
    uint32_t start = TimeGetTick();
    while ((TimeGetTick() - start) < us);
}

/**
 * @brief 毫秒延时（阻塞）
 */
void TimeDelayMs(uint32_t ms)
{
    TimeDelayUs(ms * 1000);
}