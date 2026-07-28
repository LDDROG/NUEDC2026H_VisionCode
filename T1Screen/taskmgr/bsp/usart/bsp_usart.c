/**
 * @file bsp_usart.c
 * @author neozng
 * @brief  串口bsp层的实现
 * @version beta
 * @date 2022-11-01
 *
 * @copyright Copyright (c) 2022
 *
 */
#include "bsp_usart.h"
#include "bsp_log.h"
#include <string.h>

/* usart service instance, modules' info would be recoreded here using USARTRegister() */
/* usart服务实例,所有注册了usart的模块信息会被保存在这里 */
static uint8_t idx;
static USARTInstance *usart_instance[DEVICE_USART_CNT] = {NULL};
static USARTInstance usart_instance_pool[DEVICE_USART_CNT];

/**
 * @brief 根据UART寄存器地址查找已注册实例
 */
static USARTInstance *USARTFindInstance(UART_Regs *uart)
{
    uint8_t i;

    for (i = 0U; i < idx; i++)
    {
        if (usart_instance[i]->usart_handle == uart)
        {
            return usart_instance[i];
        }
    }
    return NULL;
}

/**
 * @brief 判断实例是否使用DMA接收
 */
static uint8_t USARTUseDMAReceive(const USARTInstance *_instance)
{
    if ((_instance == NULL) || (_instance->dma_rx_channel == USART_DMA_CHANNEL_NONE))
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief 启动或重启一个实例的DMA接收
 * @note  DMA触发源由SysConfig图形化配置完成,这里不再重复配置触发源
 */
static void USARTStartDMAReceive(USARTInstance *_instance)
{
    if ((_instance == NULL) || (_instance->dma_rx_channel == USART_DMA_CHANNEL_NONE))
    {
        return;
    }

    DL_DMA_disableChannel(DMA, _instance->dma_rx_channel);

    DL_DMA_setSrcAddr(DMA,
                      _instance->dma_rx_channel,
                      (uint32_t)(uintptr_t)&(_instance->usart_handle->RXDATA));
    DL_DMA_setDestAddr(DMA,
                       _instance->dma_rx_channel,
                       (uint32_t)(uintptr_t)_instance->recv_buff);
    DL_DMA_setTransferSize(DMA, _instance->dma_rx_channel, _instance->recv_buff_size);
    DL_DMA_enableChannel(DMA, _instance->dma_rx_channel);

    _instance->is_rx_active = 0U;
    _instance->stable_ticks_us = 0U;
    _instance->last_rx_progress = 0U;
    _instance->it_rx_count = 0U;
}

/**
 * @brief 读取当前DMA已接收长度
 */
static uint16_t USARTGetReceivedSize(USARTInstance *_instance)
{
    uint16_t dma_left;
    uint16_t received;

    if (_instance == NULL)
    {
        return 0U;
    }

    if (!USARTUseDMAReceive(_instance))
    {
        if (_instance->it_rx_count > _instance->recv_buff_size)
        {
            _instance->it_rx_count = _instance->recv_buff_size;
        }

        return _instance->it_rx_count;
    }

    dma_left = DL_DMA_getTransferSize(DMA, _instance->dma_rx_channel);
    if (dma_left > _instance->recv_buff_size)
    {
        dma_left = _instance->recv_buff_size;
    }
    received = (uint16_t)_instance->recv_buff_size - dma_left;
    if (received > _instance->recv_buff_size)
    {
        received = _instance->recv_buff_size;
    }
    return received;
}

/**
 * @brief 将RX FIFO尾部尚未被DMA搬运的数据补齐到recv_buff
 */
static uint16_t USARTDrainFifoTail(USARTInstance *_instance, uint16_t received)
{
    uint8_t dropped = 0U;

    if ((_instance == NULL) || (_instance->recv_buff == NULL))
    {
        return received;
    }

    while (!DL_UART_isRXFIFOEmpty(_instance->usart_handle))
    {
        uint8_t b = DL_UART_receiveData(_instance->usart_handle);

        if (received < _instance->recv_buff_size)
        {
            _instance->recv_buff[received++] = b;
        }
        else
        {
            dropped = 1U;
        }
    }

    if (dropped)
    {
        LOGWARNING("[bsp_usart] RX frame truncated: recv_buff_size=%u", _instance->recv_buff_size);
    }

    if (!USARTUseDMAReceive(_instance))
    {
        _instance->it_rx_count = received;
    }

    return received;
}

/**
 * @brief 结束一帧并回调模块解析函数，然后重启DMA接收
 */
static void USARTFinishFrame(USARTInstance *_instance, uint16_t size)
{
    if ((_instance == NULL) || (size == 0U))
    {
        return;
    }

    _instance->last_rx_size = size;

    if (_instance->module_callback != NULL)
    {
        _instance->module_callback();
    }

    memset(_instance->recv_buff, 0, size); // 兼容旧逻辑:处理结束后清空有效区域

    if (USARTUseDMAReceive(_instance))
    {
        USARTStartDMAReceive(_instance);
    }
    else
    {
        _instance->it_rx_count = 0U;
        _instance->is_rx_active = 0U;
        _instance->stable_ticks_us = 0U;
        _instance->last_rx_progress = 0U;
    }
}

/**
 * @brief 轮询单个实例DMA剩余长度，用稳定时间判断一帧结束
 */
static void USARTPollOneInstance(USARTInstance *_instance, uint16_t elapsed_us)
{
    uint16_t received;

    if (_instance == NULL)
    {
        return;
    }

    received = USARTGetReceivedSize(_instance);

    if (received == 0U)
    {
        _instance->is_rx_active = 0U;
        _instance->stable_ticks_us = 0U;
        _instance->last_rx_progress = 0U;
        return;
    }

    if (received != _instance->last_rx_progress)
    {
        _instance->last_rx_progress = received;
        _instance->is_rx_active = 1U;
        _instance->stable_ticks_us = 0U;
        return;
    }

    if (_instance->is_rx_active)
    {
        _instance->stable_ticks_us += elapsed_us;
        if (_instance->stable_ticks_us >= _instance->rx_frame_gap_us)
        {
            USARTFinishFrame(_instance, received);
        }
    }
}

/**
 * @brief 启动串口服务,会在每个实例注册之后自动启用接收,当前实现为DMA接收,后续可能添加IT和BLOCKING接收
 *
 * @todo 串口服务会在每个实例注册之后自动启用接收,当前实现为DMA接收,后续可能添加IT和BLOCKING接收
 *       可能还要将此函数修改为extern,使得module可以控制串口的启停
 *
 * @param _instance instance owned by module,模块拥有的串口实例
 */
void USARTServiceInit(USARTInstance *_instance)
{
    if (_instance == NULL)
    {
        return;
    }

    _instance->it_rx_count = 0U;
    _instance->is_rx_active = 0U;
    _instance->stable_ticks_us = 0U;
    _instance->last_rx_progress = 0U;

    if (USARTUseDMAReceive(_instance))
    {
        /* DMA接收触发选择由图形化配置，这里只使能RX DMA事件 */
        DL_UART_enableDMAReceiveEvent(_instance->usart_handle, DL_UART_DMA_INTERRUPT_RX);
        DL_UART_disableInterrupt(_instance->usart_handle, DL_UART_INTERRUPT_RX);
        USARTStartDMAReceive(_instance);
    }
    else
    {
        DL_UART_disableDMAReceiveEvent(_instance->usart_handle, DL_UART_DMA_INTERRUPT_RX);
        DL_UART_enableInterrupt(_instance->usart_handle,
                                DL_UART_INTERRUPT_RX |
                                    DL_UART_INTERRUPT_RX_TIMEOUT_ERROR |
                                    DL_UART_INTERRUPT_OVERRUN_ERROR |
                                    DL_UART_INTERRUPT_BREAK_ERROR |
                                    DL_UART_INTERRUPT_PARITY_ERROR |
                                    DL_UART_INTERRUPT_FRAMING_ERROR |
                                    DL_UART_INTERRUPT_NOISE_ERROR);
    }
}

USARTInstance *USARTRegister(USART_Init_Config_s *init_config)
{
    uint32_t frame_gap_us;
    USARTInstance *instance;

    if (init_config == NULL)
    {
        return NULL;
    }

    if (idx >= DEVICE_USART_CNT) // 超过最大实例数
        while (1)
            LOGERROR("[bsp_usart] USART exceed max instance count!");

    for (uint8_t i = 0; i < idx; i++) // 检查是否已经注册过
        if (usart_instance[i]->usart_handle == init_config->usart_handle)
            while (1)
                LOGERROR("[bsp_usart] USART instance already registered!");

    instance = &usart_instance_pool[idx];
    memset(instance, 0, sizeof(USARTInstance));

    instance->recv_buff = init_config->recv_buff;
    instance->usart_handle = init_config->usart_handle;
    instance->recv_buff_size = init_config->recv_buff_size;
    instance->module_callback = init_config->module_callback;
    instance->dma_rx_channel = init_config->dma_rx_channel;
    instance->dma_tx_channel = init_config->dma_tx_channel;

    frame_gap_us = init_config->rx_frame_gap_us;
    if (frame_gap_us == 0U)
    {
        frame_gap_us = USART_RX_FRAME_GAP_US_DEFAULT;
    }
    instance->rx_frame_gap_us = frame_gap_us;

    if (instance->recv_buff_size > USART_RXBUFF_LIMIT)
    {
        instance->recv_buff_size = USART_RXBUFF_LIMIT;
        LOGWARNING("[bsp_usart] recv_buff_size clamp to USART_RXBUFF_LIMIT");
    }

    if (instance->recv_buff == NULL)
    {
        while (1)
            LOGERROR("[bsp_usart] recv_buff cannot be NULL!");
    }

    if (instance->recv_buff_size == 0U)
    {
        while (1)
            LOGERROR("[bsp_usart] recv_buff_size cannot be zero!");
    }

    usart_instance[idx++] = instance;
    USARTServiceInit(instance);
    return instance;
}

/**
 * @brief 在IT发送模式下尽量向TX FIFO搬运数据
 */
static void USARTKickITTx(USARTInstance *_instance)
{
    if ((_instance == NULL) || (!_instance->tx_it_busy))
    {
        return;
    }

    while (_instance->tx_it_index < _instance->tx_it_size)
    {
        if (!DL_UART_transmitDataCheck(_instance->usart_handle,
                                       _instance->tx_it_buf[_instance->tx_it_index]))
        {
            break;
        }

        _instance->tx_it_index++;
    }

    if (_instance->tx_it_index >= _instance->tx_it_size)
    {
        DL_UART_disableInterrupt(_instance->usart_handle, DL_UART_INTERRUPT_TX);
        _instance->tx_it_busy = 0U;
        _instance->tx_it_buf = NULL;
        _instance->tx_it_size = 0U;
        _instance->tx_it_index = 0U;
    }
}

/**
 * @brief 串口发送统一接口,支持阻塞/轮询发送和DMA发送
 */
/* @todo 当前仅进行了形式上的封装,后续要进一步考虑是否将module的行为与bsp完全分离 */
void USARTSend(USARTInstance *_instance, uint8_t *send_buf, uint16_t send_size, USART_TRANSFER_MODE mode)
{
    uint16_t i;

    if ((_instance == NULL) || (send_buf == NULL) || (send_size == 0U))
    {
        return;
    }

    switch (mode)
    {
    case USART_TRANSFER_BLOCKING:
        for (i = 0U; i < send_size; i++)
        {
            DL_UART_transmitDataBlocking(_instance->usart_handle, send_buf[i]);
        }
        break;
    case USART_TRANSFER_IT:
        if (_instance->tx_it_busy)
        {
            LOGWARNING("[bsp_usart] IT TX busy, drop this send request");
            break;
        }

        _instance->tx_it_buf = send_buf;
        _instance->tx_it_size = send_size;
        _instance->tx_it_index = 0U;
        _instance->tx_it_busy = 1U;

        USARTKickITTx(_instance);
        if (_instance->tx_it_busy)
        {
            DL_UART_enableInterrupt(_instance->usart_handle, DL_UART_INTERRUPT_TX);
        }

        break;
    case USART_TRANSFER_DMA:
        if (_instance->dma_tx_channel == USART_DMA_CHANNEL_NONE)
        {
            for (i = 0U; i < send_size; i++)
            {
                DL_UART_transmitDataBlocking(_instance->usart_handle, send_buf[i]);
            }
            break;
        }

        DL_DMA_disableChannel(DMA, _instance->dma_tx_channel);
        DL_DMA_setSrcAddr(DMA, _instance->dma_tx_channel, (uint32_t)(uintptr_t)send_buf);
        DL_DMA_setDestAddr(DMA,
                           _instance->dma_tx_channel,
                           (uint32_t)(uintptr_t)&(_instance->usart_handle->TXDATA));
        DL_DMA_setTransferSize(DMA, _instance->dma_tx_channel, send_size);
        DL_UART_enableDMATransmitEvent(_instance->usart_handle);
        DL_DMA_enableChannel(DMA, _instance->dma_tx_channel);
        break;
    default:
        while (1)
            ; // illegal mode! check your code context! 检查定义instance的代码上下文,可能出现指针越界
        break;
    }
}

/**
 * @brief 检查串口与DMA发送状态,用于连续发送前的就绪判断
 */
/* 串口发送时,gstate会被设为BUSY_TX */
uint8_t USARTIsReady(USARTInstance *_instance)
{
    if ((_instance == NULL) || (_instance->usart_handle == NULL))
    {
        return 0U;
    }

    if (DL_UART_isBusy(_instance->usart_handle))
        return 0;

    if (_instance->tx_it_busy)
    {
        return 0U;
    }

    if (_instance->dma_tx_channel != USART_DMA_CHANNEL_NONE)
    {
        if (DL_DMA_isChannelEnabled(DMA, _instance->dma_tx_channel) &&
            (DL_DMA_getTransferSize(DMA, _instance->dma_tx_channel) > 0U))
        {
            return 0U;
        }
    }

    return 1U;
}

/**
 * @brief 每次dma/idle中断发生时，都会调用此函数.对于每个uart实例会调用对应的回调进行进一步的处理
 *        例如:视觉协议解析/遥控器解析/裁判系统解析
 *
 * @note  通过__HAL_DMA_DISABLE_IT(huart->hdmarx,DMA_IT_HT)关闭dma half transfer中断防止两次进入HAL_UARTEx_RxEventCallback()
 *        这是HAL库的一个设计失误,发生DMA传输完成/半完成以及串口IDLE中断都会触发HAL_UARTEx_RxEventCallback()
 *        我们只希望处理，因此直接关闭DMA半传输中断第一种和第三种情况
 *
 * @param huart 发生中断的串口
 * @param Size 此次接收到的总数居量,暂时没用
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    USARTInstance *ins = USARTFindInstance(huart);
    uint16_t received = 0U;

    (void)Size;

    if (ins == NULL)
    {
        return;
    }

    received = USARTGetReceivedSize(ins);
    received = USARTDrainFifoTail(ins, received);

    USARTFinishFrame(ins, received);
}

/**
 * @brief 当串口发送/接收出现错误时,会调用此函数,此时这个函数要做的就是重新启动接收
 *
 * @note  最常见的错误:奇偶校验/溢出/帧错误
 *
 * @param huart 发生错误的串口
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    for (uint8_t i = 0; i < idx; ++i)
    {
        if (huart == usart_instance[i]->usart_handle)
        {
            if (USARTUseDMAReceive(usart_instance[i]))
            {
                USARTStartDMAReceive(usart_instance[i]);
            }
            else
            {
                while (!DL_UART_isRXFIFOEmpty(usart_instance[i]->usart_handle))
                {
                    (void)DL_UART_receiveData(usart_instance[i]->usart_handle);
                }

                usart_instance[i]->it_rx_count = 0U;
                usart_instance[i]->is_rx_active = 0U;
                usart_instance[i]->stable_ticks_us = 0U;
                usart_instance[i]->last_rx_progress = 0U;
            }

            LOGWARNING("[bsp_usart] USART error callback triggered, instance idx [%d]", i);
            return;
        }
    }
}



void USARTServiceTickUs(uint16_t elapsed_us)
{
    uint8_t i;

    if (elapsed_us == 0U)
    {
        return;
    }

    for (i = 0U; i < idx; i++)
    {
        USARTPollOneInstance(usart_instance[i], elapsed_us);
    }
}

/**
 * @brief UART IRQ统一分发
 */
static void USARTHandleUARTIRQ(UART_Regs *uart)
{
    USARTInstance *ins;
    DL_UART_IIDX iidx;

    iidx = DL_UART_getPendingInterrupt(uart);
    switch (iidx)
    {
    case DL_UART_IIDX_TX:
        ins = USARTFindInstance(uart);
        if ((ins != NULL) && ins->tx_it_busy)
        {
            USARTKickITTx(ins);
        }
        else
        {
            DL_UART_disableInterrupt(uart, DL_UART_INTERRUPT_TX);
        }
        break;
    case DL_UART_IIDX_RX:
        ins = USARTFindInstance(uart);
        if ((ins != NULL) && (!USARTUseDMAReceive(ins)))
        {
            uint16_t received = USARTGetReceivedSize(ins);
            USARTDrainFifoTail(ins, received);
        }
        break;
    case DL_UART_IIDX_RX_TIMEOUT_ERROR:
        DL_UART_clearInterruptStatus(uart, DL_UART_INTERRUPT_RX_TIMEOUT_ERROR);
        HAL_UARTEx_RxEventCallback(uart, 0U);
        break;
    case DL_UART_IIDX_OVERRUN_ERROR:
        DL_UART_clearInterruptStatus(uart, DL_UART_INTERRUPT_OVERRUN_ERROR);
        HAL_UART_ErrorCallback(uart);
        break;
    case DL_UART_IIDX_BREAK_ERROR:
        DL_UART_clearInterruptStatus(uart, DL_UART_INTERRUPT_BREAK_ERROR);
        HAL_UART_ErrorCallback(uart);
        break;
    case DL_UART_IIDX_PARITY_ERROR:
        DL_UART_clearInterruptStatus(uart, DL_UART_INTERRUPT_PARITY_ERROR);
        HAL_UART_ErrorCallback(uart);
        break;
    case DL_UART_IIDX_FRAMING_ERROR:
        DL_UART_clearInterruptStatus(uart, DL_UART_INTERRUPT_FRAMING_ERROR);
        HAL_UART_ErrorCallback(uart);
        break;
    case DL_UART_IIDX_NOISE_ERROR:
        DL_UART_clearInterruptStatus(uart, DL_UART_INTERRUPT_NOISE_ERROR);
        HAL_UART_ErrorCallback(uart);
        break;
    default:
        break;
    }
}

void UART0_IRQHandler(void)
{
    USARTHandleUARTIRQ(UART0);
}


void UART1_IRQHandler(void)
{
    USARTHandleUARTIRQ(UART1);
}

void UART2_IRQHandler(void)
{
    USARTHandleUARTIRQ(UART2);
}

void UART3_IRQHandler(void)
{
    USARTHandleUARTIRQ(UART3);
}
