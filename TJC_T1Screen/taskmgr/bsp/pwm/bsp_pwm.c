#include "bsp_pwm.h"
#include "stdlib.h"
#include "string.h"

// 配合中断以及初始化
static uint8_t idx;
static PWMInstance *pwm_instance[PWM_DEVICE_CNT] = {NULL}; // 所有的pwm instance保存于此,用于callback时判断中断来源
static uint32_t PWMSelectTclk(TIM_HandleTypeDef *htim );
/**
 * @brief pwm dma传输完成回调函数
 *
 * @param htim 发生中断的定时器句柄
 */
/*
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    for (uint8_t i = 0; i < idx; i++)
    { // 来自同一个定时器的中断且通道相同
        if (pwm_instance[i]->htim == htim && htim->Channel == (1<<(pwm_instance[i]->channel/4)))
        {
            if (pwm_instance[i]->callback) // 如果有回调函数
                pwm_instance[i]->callback(pwm_instance[i]);
            return; // 一次只能有一个通道的中断,所以直接返回
        }
    }
}
*/
PWMInstance *PWMRegister(PWM_Init_Config_s *config)
{
    if (idx >= PWM_DEVICE_CNT) // 超过最大实例数,考虑增加或查看是否有内存泄漏
        while (1)
            ;
    PWMInstance *pwm = (PWMInstance *)malloc(sizeof(PWMInstance));
    memset(pwm, 0, sizeof(PWMInstance));

    pwm->htim = config->htim;
    pwm->channel = config->channel;
    pwm->period = config->period;
    pwm->dutyratio = config->dutyratio;
    pwm->callback = config->callback;
    pwm->id = config->id;
    pwm->tclk = PWMSelectTclk(pwm->htim);

    pwm->arr = DL_Timer_getLoadValue(pwm->htim);
    // 启动PWM
    //HAL_TIM_PWM_Start(pwm->htim, pwm->channel);
    PWMStart(pwm);
    PWMSetPeriod(pwm, pwm->period);
    PWMSetDutyRatio(pwm, pwm->dutyratio);
    pwm_instance[idx++] = pwm;
    return pwm;
}

/* 只是对HAL的函数进行了形式上的封装 */
void PWMStart(PWMInstance *pwm)
{
    //HAL_TIM_PWM_Start(pwm->htim, pwm->channel);
    DL_Timer_startCounter(pwm->htim);
}

/* 只是对HAL的函数进行了形式上的封装 */
void PWMStop(PWMInstance *pwm)
{
    //HAL_TIM_PWM_Stop(pwm->htim, pwm->channel);
    DL_Timer_stopCounter(pwm->htim);
}

/*
 * @brief 设置pwm周期
 *
 * @param pwm pwm实例
 * @param period 周期 单位 s
 */
void PWMSetPeriod(PWMInstance *pwm, float period)
{
    // 获取当前时钟配置
    DL_Timer_ClockConfig clockConfig;
    DL_Timer_getClockConfig(pwm->htim, &clockConfig);
    
    // 计算实际时钟频率
    // timerClkFreq = timerClkSrc / (divideRatio * (prescale + 1))
    uint32_t timerClkSrc = pwm->tclk;  // 源时钟频率
    
    // 分频比映射：DL_TIMER_CLOCK_DIVIDE_1 = 0, DL_TIMER_CLOCK_DIVIDE_2 = 1, ...
    uint32_t divideRatio = 1U << clockConfig.divideRatio;
    uint32_t prescale = clockConfig.prescale;
    
    // 计算实际定时器时钟频率
    uint32_t timerClkFreq = timerClkSrc / (divideRatio * (prescale + 1));
    
    // 计算ARR值: ARR = period * timerClkFreq
    uint32_t arr = (uint32_t)(period * timerClkFreq);
    
    // 限制ARR范围（16位或32位定时器）
    if (arr > 0xFFFF && (pwm->htim != TIMG12))
    {
        arr = 0xFFFF;  // TIMG是16位定时器
    }
    else if (arr > 0xFFFFFFFF)
    {
        arr = 0xFFFFFFFF;  // 32位限制
    }
    if (arr < 1) arr = 1;  // 最小值为1
    
    DL_Timer_setLoadValue(pwm->htim, arr);
    pwm->arr = arr;
    pwm->period = period;
    
    // 更新占空比（因为ARR改变了）
    PWMSetDutyRatio(pwm, pwm->dutyratio);
}
/*
    * @brief 设置pwm占空比
    *
    * @param pwm pwm实例
    * @param dutyratio 占空比 0~1
*/
void PWMSetDutyRatio(PWMInstance *pwm, float dutyratio)
{
    //__HAL_TIM_SetCompare(pwm->htim, pwm->channel, dutyratio * (pwm->htim->Instance->ARR));
    // 限制占空比范围
    if (dutyratio < 0.0f) dutyratio = 0.0f;
    if (dutyratio > 1.0f) dutyratio = 1.0f;
    
    uint32_t compare_value = (uint32_t)(dutyratio * pwm->arr);
    DL_Timer_setCaptureCompareValue(pwm->htim, compare_value, pwm->channel);
    pwm->dutyratio = dutyratio;    
}

/* 只是对HAL的函数进行了形式上的封装 */
void PWMStartDMA(PWMInstance *pwm, uint32_t *pData, uint32_t Size)
{
    //HAL_TIM_PWM_Start_DMA(pwm->htim, pwm->channel, pData, Size);
}

// 设置pwm对应定时器时钟源频率
static uint32_t PWMSelectTclk(TIM_HandleTypeDef *htim)
{
    uint32_t clk_freq = 0;
    
    // 获取时钟配置以确定时钟源
    DL_Timer_ClockConfig clockConfig;
    DL_Timer_getClockConfig(htim, &clockConfig);
    
    // 根据时钟源选择获取对应的频率
    switch (clockConfig.clockSel)
    {
        case DL_TIMER_CLOCK_BUSCLK:
            // 总线时钟，通常是MCLK
            clk_freq = CPUCLK_FREQ;
            break;
            
            
        case DL_TIMER_CLOCK_MFCLK:
            // 中频时钟 4MHz
            clk_freq = 4000000UL;
            break;

        default:
            clk_freq = CPUCLK_FREQ;
            break;
    }
    
    return clk_freq;
}
