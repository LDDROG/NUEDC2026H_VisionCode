#include "bsp_gpio.h"
#include <stdlib.h>
#include <string.h>

static uint8_t idx = 0U;
static GPIOInstance *gpio_instance[GPIO_MX_DEVICE_NUM] = {0};

void HAL_GPIO_EXTI_Callback(int GPIO_Pin)
{
    GPIOInstance *gpio;
    size_t i;

    for (i = 0; i < idx; i++)
    {
        gpio = gpio_instance[i];
        if ((gpio != NULL) &&
            (gpio->GPIO_Pin == GPIO_Pin) &&
            (gpio->gpio_model_callback != NULL))
        {
            gpio->gpio_model_callback(gpio);
            return;
        }
    }
}
static void BSP_GPIO_DispatchIRQ(GPIO_Regs *port)
{
    uint32_t pending;
    int pinMask;

    /* 读取该端口“已使能”的中断状态位 */
    pending = DL_GPIO_getEnabledInterruptStatus(port, 0xFFFFFFFFU);
    if (pending == 0U) {
        return;
    }

    /* 先清标志，避免在回调里做较长处理时重复进中断 */
    DL_GPIO_clearInterruptStatus(port, pending);

    /* 把每一位拆出来，映射到你现有 HAL_GPIO_EXTI_Callback(GPIO_Pin) */
    while (pending != 0U) {
        pinMask = pending & (0U - pending);   /* 取最低位 1 */
        HAL_GPIO_EXTI_Callback(pinMask);
        pending &= ~pinMask;
    }
}

void GROUP1_IRQHandler(void)
{
    /* 常见情况：GPIOA/ GPIOB 都在 GROUP1 */
    BSP_GPIO_DispatchIRQ(GPIOA);
    BSP_GPIO_DispatchIRQ(GPIOB);
}

GPIOInstance *GPIORegister(GPIO_Init_Config_s *GPIO_config)
{
    GPIOInstance *ins;

    if ((GPIO_config == NULL) || (idx >= GPIO_MX_DEVICE_NUM))
    {
        return NULL;
    }

    ins = (GPIOInstance *)malloc(sizeof(GPIOInstance));
    if (ins == NULL)
    {
        return NULL;
    }

    memset(ins, 0, sizeof(GPIOInstance));

    ins->GPIOx = GPIO_config->GPIOx;
    ins->GPIO_Pin = GPIO_config->GPIO_Pin;
    ins->pin_state = GPIO_config->pin_state;
    ins->exti_mode = GPIO_config->exti_mode;
    ins->id = GPIO_config->id;
    ins->gpio_model_callback = GPIO_config->gpio_model_callback;

    gpio_instance[idx++] = ins;
    return ins;
}

void GPIOToggle(GPIOInstance *_instance)
{
    if (_instance == NULL)
    {
        return;
    }
    DL_GPIO_togglePins(_instance->GPIOx, (uint32_t)_instance->GPIO_Pin);
}

void GPIOSet(GPIOInstance *_instance)
{
    if (_instance == NULL)
    {
        return;
    }
    DL_GPIO_setPins(_instance->GPIOx, (uint32_t)_instance->GPIO_Pin);
}

void GPIOReset(GPIOInstance *_instance)
{
    if (_instance == NULL)
    {
        return;
    }
    DL_GPIO_clearPins(_instance->GPIOx, (uint32_t)_instance->GPIO_Pin);
}

GPIO_PinState GPIORead(GPIOInstance *_instance)
{
    uint32_t value;

    if (_instance == NULL)
    {
        return GPIO_PIN_RESET;
    }

    value = DL_GPIO_readPins(_instance->GPIOx, (uint32_t)_instance->GPIO_Pin);
    return (value != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}
