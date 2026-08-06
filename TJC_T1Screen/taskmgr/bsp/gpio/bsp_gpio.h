#ifndef BSP_GPIO_H
#define BSP_GPIO_H

#include <stdint.h>
#include "ti_msp_dl_config.h"

#define GPIO_MX_DEVICE_NUM 10

/* STM32 HAL compatible type aliases for API stability */
typedef GPIO_Regs GPIO_TypeDef;

typedef enum
{
    GPIO_PIN_RESET = 0U,
    GPIO_PIN_SET
} GPIO_PinState;

/**
 * @brief 用于判断中断来源,注意和配置一致
 */
typedef enum
{
    GPIO_EXTI_MODE_RISING,
    GPIO_EXTI_MODE_FALLING,
    GPIO_EXTI_MODE_RISING_FALLING,
    GPIO_EXTI_MODE_NONE,
} GPIO_EXTI_MODE_e;

/**
 * @brief GPIO实例结构体定义
 */
typedef struct tmpgpio
{
    GPIO_TypeDef *GPIOx;
    GPIO_PinState pin_state;
    GPIO_EXTI_MODE_e exti_mode;
    int GPIO_Pin; /* 这里继续使用 bitmask 语义 */
    void (*gpio_model_callback)(struct tmpgpio *);
    void *id;
} GPIOInstance;

/**
 * @brief GPIO初始化配置结构体定义
 */
typedef struct
{
    GPIO_TypeDef *GPIOx;
    GPIO_PinState pin_state;
    GPIO_EXTI_MODE_e exti_mode;
    int GPIO_Pin; /* bitmask */

    void (*gpio_model_callback)(GPIOInstance *);
    void *id;
} GPIO_Init_Config_s;

/**
 * @brief 注册GPIO实例
 */
GPIOInstance *GPIORegister(GPIO_Init_Config_s *GPIO_config);

/**
 * @brief GPIO API,切换GPIO电平
 */
void GPIOToggle(GPIOInstance *_instance);

/**
 * @brief 设置GPIO电平
 */
void GPIOSet(GPIOInstance *_instance);

/**
 * @brief 复位GPIO电平
 */
void GPIOReset(GPIOInstance *_instance);

/**
 * @brief 读取GPIO电平
 */
GPIO_PinState GPIORead(GPIOInstance *_instance);

/**
 * @brief 与原工程兼容的回调入口（在 MSPM0 对应的 GPIO ISR 中调用）
 */
void HAL_GPIO_EXTI_Callback(int GPIO_Pin);

#endif