#include "bsp_iic.h"
#include <stdlib.h>
#include <string.h>

#define IIC_BUSY_WAIT_LIMIT (2000000U)

static uint8_t idx = 0;
static IICInstance *iic_instance[MX_IIC_SLAVE_CNT] = {NULL};

/**
 * @brief 根据I2C实例获取IRQ号
 * @param handle I2C外设基地址
 * @return IRQn_Type 目标中断号
 */
static IRQn_Type iic_get_irqn(I2C_HandleTypeDef *handle)
{
    if (handle == I2C0) {
        return I2C0_INT_IRQn;
    }
    if (handle == I2C1) {
        return I2C1_INT_IRQn;
    }
    return NonMaskableInt_IRQn;
}

/**
 * @brief 根据I2C实例和方向获取DMA触发源
 * @param handle I2C外设基地址
 * @param is_tx true表示TX, false表示RX
 * @param trigger_out 输出的DMA触发编号
 * @return bool true表示获取成功
 */
static bool iic_get_dma_trigger(const I2C_HandleTypeDef *handle, bool is_tx,
    uint8_t *trigger_out)
{
    if (trigger_out == NULL) {
        return false;
    }

    if (handle == I2C0) {
        *trigger_out = is_tx ? DMA_I2C0_TX_TRIG : DMA_I2C0_RX_TRIG;
        return true;
    }
    if (handle == I2C1) {
        *trigger_out = is_tx ? DMA_I2C1_TX_TRIG : DMA_I2C1_RX_TRIG;
        return true;
    }

    return false;
}

/**
 * @brief 等待I2C控制器进入IDLE状态
 * @param iic IIC实例
 * @return bool true表示等待成功
 */
static bool iic_wait_idle(const IICInstance *iic)
{
    uint32_t timeout = IIC_BUSY_WAIT_LIMIT;
    while (((DL_I2C_getControllerStatus(iic->handle) & DL_I2C_CONTROLLER_STATUS_IDLE) == 0U) &&
           (timeout > 0U)) {
        timeout--;
    }
    return (timeout > 0U);
}

/**
 * @brief 等待I2C总线空闲
 * @param iic IIC实例
 * @return bool true表示等待成功
 */
static bool iic_wait_bus_free(const IICInstance *iic)
{
    uint32_t timeout = IIC_BUSY_WAIT_LIMIT;
    while (((DL_I2C_getControllerStatus(iic->handle) & DL_I2C_CONTROLLER_STATUS_BUSY_BUS) != 0U) &&
           (timeout > 0U)) {
        timeout--;
    }
    return (timeout > 0U);
}

/**
 * @brief 通过I2C外设句柄查找实例
 * @param handle I2C外设基地址
 * @return IICInstance* 匹配到的实例
 */
static IICInstance *iic_find_instance_by_handle(I2C_HandleTypeDef *handle)
{
    uint8_t i;
    for (i = 0; i < idx; i++) {
        if ((iic_instance[i] != NULL) && (iic_instance[i]->handle == handle)) {
            return iic_instance[i];
        }
    }
    return NULL;
}

/**
 * @brief 设置一次传输上下文
 * @param iic IIC实例
 * @param buffer 数据缓冲区
 * @param size 数据长度
 * @param is_tx true表示发送, false表示接收
 * @param use_dma 是否使用DMA
 * @param seq_mode 序列传输模式
 */
static void iic_set_transfer_context(IICInstance *iic, uint8_t *buffer,
    uint16_t size, bool is_tx, bool use_dma, IIC_Seq_Mode_e seq_mode)
{
    iic->use_dma = use_dma;
    iic->seq_mode = seq_mode;
    iic->transfer_busy = true;
    if (is_tx) {
        iic->tx_buffer = buffer;
        iic->tx_len = size;
        iic->tx_count = 0;
    } else {
        iic->rx_buffer = buffer;
        iic->rx_len = size;
        iic->rx_count = 0;
    }
}

/**
 * @brief 清除当前传输状态
 * @param iic IIC实例
 */
static void iic_clear_transfer_state(IICInstance *iic)
{
    iic->transfer_busy = false;
    iic->tx_buffer = NULL;
    iic->rx_buffer = NULL;
    iic->tx_len = 0;
    iic->rx_len = 0;
    iic->tx_count = 0;
    iic->rx_count = 0;
    iic->use_dma = false;
    iic->seq_mode = IIC_SEQ_RELEASE;
}

/**
 * @brief 结束一次成功传输并触发回调
 * @param iic IIC实例
 */
static void iic_finalize_success(IICInstance *iic)
{
    iic_clear_transfer_state(iic);
    if (iic->callback != NULL) {
        iic->callback(iic);
    }
}

/**
 * @brief 结束一次失败传输并清空FIFO
 * @param iic IIC实例
 */
static void iic_finalize_error(IICInstance *iic)
{
    DL_I2C_flushControllerTXFIFO(iic->handle);
    DL_I2C_flushControllerRXFIFO(iic->handle);
    iic_clear_transfer_state(iic);
}

/**
 * @brief 启动I2C主机传输
 * @param iic IIC实例
 * @param is_tx true表示发送, false表示接收
 * @param size 传输长度
 * @param seq_mode 序列传输模式
 */
static void iic_start_transfer(IICInstance *iic, bool is_tx, uint16_t size,
    IIC_Seq_Mode_e seq_mode)
{
    DL_I2C_CONTROLLER_STOP stop_mode =
        (seq_mode == IIC_SEQ_HOLDON) ? DL_I2C_CONTROLLER_STOP_DISABLE : DL_I2C_CONTROLLER_STOP_ENABLE;
    DL_I2C_startControllerTransferAdvanced(iic->handle, iic->dev_address,
        is_tx ? DL_I2C_CONTROLLER_DIRECTION_TX : DL_I2C_CONTROLLER_DIRECTION_RX,
        size, DL_I2C_CONTROLLER_START_ENABLE, stop_mode, DL_I2C_CONTROLLER_ACK_ENABLE);
}

/**
 * @brief 将待发数据写入TX FIFO
 * @param iic IIC实例
 */
static void iic_feed_tx_fifo(IICInstance *iic)
{
    while ((iic->tx_count < iic->tx_len) &&
           (DL_I2C_isControllerTXFIFOFull(iic->handle) == false)) {
        DL_I2C_transmitControllerData(iic->handle, iic->tx_buffer[iic->tx_count]);
        iic->tx_count++;
    }
}

/**
 * @brief 将RX FIFO中的数据取出到缓冲区
 * @param iic IIC实例
 */
static void iic_drain_rx_fifo(IICInstance *iic)
{
    while ((iic->rx_count < iic->rx_len) &&
           (DL_I2C_isControllerRXFIFOEmpty(iic->handle) == false)) {
        iic->rx_buffer[iic->rx_count] = DL_I2C_receiveControllerData(iic->handle);
        iic->rx_count++;
    }
}

/**
 * @brief 配置并启动单DMA通道用于I2C收发
 * @param iic IIC实例
 * @param buffer 数据缓冲区
 * @param size 传输长度
 * @param is_tx true表示发送, false表示接收
 * @return bool true表示配置成功
 */
static bool iic_prepare_dma(IICInstance *iic, uint8_t *buffer, uint16_t size,
    bool is_tx)
{
    uint8_t trigger;

    if ((iic == NULL) || (buffer == NULL) || (size == 0U) ||
        (iic->dma_channel == IIC_DMA_CHANNEL_NONE)) {
        return false;
    }
    if (!iic_get_dma_trigger(iic->handle, is_tx, &trigger)) {
        return false;
    }

    DL_DMA_disableChannel(DMA, iic->dma_channel);
    DL_DMA_setTrigger(DMA, iic->dma_channel, trigger, DL_DMA_TRIGGER_TYPE_EXTERNAL);

    if (is_tx) {
        DL_DMA_setSrcAddr(DMA, iic->dma_channel, (uint32_t) (uintptr_t) buffer);
        DL_DMA_setDestAddr(DMA, iic->dma_channel,
            (uint32_t) (uintptr_t) (&iic->handle->MASTER.MTXDATA));
        DL_DMA_setSrcIncrement(DMA, iic->dma_channel, DL_DMA_ADDR_INCREMENT);
        DL_DMA_setDestIncrement(DMA, iic->dma_channel, DL_DMA_ADDR_UNCHANGED);
    } else {
        DL_DMA_setSrcAddr(DMA, iic->dma_channel,
            (uint32_t) (uintptr_t) (&iic->handle->MASTER.MRXDATA));
        DL_DMA_setDestAddr(DMA, iic->dma_channel, (uint32_t) (uintptr_t) buffer);
        DL_DMA_setSrcIncrement(DMA, iic->dma_channel, DL_DMA_ADDR_UNCHANGED);
        DL_DMA_setDestIncrement(DMA, iic->dma_channel, DL_DMA_ADDR_INCREMENT);
    }

    DL_DMA_setTransferSize(DMA, iic->dma_channel, size);
    DL_DMA_enableChannel(DMA, iic->dma_channel);

    return true;
}

/**
 * @brief 注册IIC实例
 * @param conf 初始化配置
 * @return IICInstance* 注册成功返回实例指针, 失败返回NULL
 */
IICInstance *IICRegister(IIC_Init_Config_s *conf)
{
    IICInstance *instance;
    IRQn_Type irqn;

    if ((conf == NULL) || (conf->handle == NULL) || (idx >= MX_IIC_SLAVE_CNT)) {
        return NULL;
    }

    instance = (IICInstance *) malloc(sizeof(IICInstance));
    if (instance == NULL) {
        return NULL;
    }
    memset(instance, 0, sizeof(IICInstance));

    instance->dev_address = (uint8_t) (conf->dev_address & 0x7FU);
    instance->callback = conf->callback;
    instance->work_mode = conf->work_mode;
    instance->handle = conf->handle;
    instance->id = conf->id;
    instance->dma_channel = conf->dma_channel;
    instance->seq_mode = IIC_SEQ_RELEASE;

    // TX/RX数据寄存器虽然不同(MTXDATA/MRXDATA), 但I2C是半双工, 可单DMA通道分时复用。
    DL_I2C_enableDMAEvent(conf->handle, DL_I2C_EVENT_ROUTE_1,
        DL_I2C_DMA_INTERRUPT_CONTROLLER_RXFIFO_TRIGGER);
    DL_I2C_enableDMAEvent(conf->handle, DL_I2C_EVENT_ROUTE_2,
        DL_I2C_DMA_INTERRUPT_CONTROLLER_TXFIFO_TRIGGER);

    DL_I2C_enableInterrupt(conf->handle,
        DL_I2C_INTERRUPT_CONTROLLER_RX_DONE |
            DL_I2C_INTERRUPT_CONTROLLER_TX_DONE |
            DL_I2C_INTERRUPT_CONTROLLER_RXFIFO_TRIGGER |
            DL_I2C_INTERRUPT_CONTROLLER_RXFIFO_FULL |
            DL_I2C_INTERRUPT_CONTROLLER_TXFIFO_TRIGGER |
            DL_I2C_INTERRUPT_CONTROLLER_TXFIFO_EMPTY |
            DL_I2C_INTERRUPT_CONTROLLER_EVENT1_DMA_DONE |
            DL_I2C_INTERRUPT_CONTROLLER_EVENT2_DMA_DONE);

    irqn = iic_get_irqn(conf->handle);
    if ((irqn == I2C0_INT_IRQn) || (irqn == I2C1_INT_IRQn)) {
        NVIC_EnableIRQ(irqn);
    }

    iic_instance[idx] = instance;
    idx++;
    return instance;
}

/**
 * @brief 设置IIC工作模式
 * @param iic 要设置的iic实例
 * @param mode 工作模式
 */
void IICSetMode(IICInstance *iic, IIC_Work_Mode_e mode)
{
    if (iic == NULL) {
        return;
    }
    iic->work_mode = mode;
}

/**
 * @brief IIC发送数据
 * @param iic iic实例
 * @param data 待发送数据首地址
 * @param size 发送长度
 * @param seq_mode 序列传输模式
 */
void IICTransmit(IICInstance *iic, uint8_t *data, uint16_t size, IIC_Seq_Mode_e seq_mode)
{
    if ((iic == NULL) || (data == NULL) || (size == 0U)) {
        return;
    }
    if ((seq_mode != IIC_SEQ_RELEASE) && (seq_mode != IIC_SEQ_HOLDON)) {
        return;
    }
    if (iic->transfer_busy) {
        return;
    }

    switch (iic->work_mode)
    {
    case IIC_BLOCK_MODE:
        if ((seq_mode != IIC_SEQ_RELEASE) || !iic_wait_idle(iic)) {
            return;
        }

        iic_set_transfer_context(iic, data, size, true, false, seq_mode);
        iic_feed_tx_fifo(iic);
        iic_start_transfer(iic, true, size, IIC_SEQ_RELEASE);
        while (iic->tx_count < iic->tx_len) {
            iic_feed_tx_fifo(iic);
        }
        if (iic_wait_bus_free(iic) && iic_wait_idle(iic)) {
            iic_finalize_success(iic);
        } else {
            iic_finalize_error(iic);
        }
        break;

    case IIC_IT_MODE:
        if (!iic_wait_idle(iic)) {
            return;
        }
        iic_set_transfer_context(iic, data, size, true, false, seq_mode);
        iic_feed_tx_fifo(iic);
        iic_start_transfer(iic, true, size, seq_mode);
        break;

    case IIC_DMA_MODE:
        if (!iic_wait_idle(iic)) {
            return;
        }
        if (!iic_prepare_dma(iic, data, size, true)) {
            return;
        }

        iic_set_transfer_context(iic, data, size, true, true, seq_mode);
        iic_start_transfer(iic, true, size, seq_mode);
        break;

    default:
        break;
    }
}

/**
 * @brief IIC接收数据
 * @param iic iic实例
 * @param data 接收缓冲区首地址
 * @param size 接收长度
 * @param seq_mode 序列传输模式
 */
void IICReceive(IICInstance *iic, uint8_t *data, uint16_t size, IIC_Seq_Mode_e seq_mode)
{
    if ((iic == NULL) || (data == NULL) || (size == 0U)) {
        return;
    }
    if ((seq_mode != IIC_SEQ_RELEASE) && (seq_mode != IIC_SEQ_HOLDON)) {
        return;
    }
    if (iic->transfer_busy) {
        return;
    }

    switch (iic->work_mode)
    {
    case IIC_BLOCK_MODE:
        if ((seq_mode != IIC_SEQ_RELEASE) || !iic_wait_idle(iic)) {
            return;
        }

        iic_set_transfer_context(iic, data, size, false, false, seq_mode);
        iic_start_transfer(iic, false, size, IIC_SEQ_RELEASE);
        while (iic->rx_count < iic->rx_len) {
            iic_drain_rx_fifo(iic);
        }
        if (iic_wait_idle(iic)) {
            iic_finalize_success(iic);
        } else {
            iic_finalize_error(iic);
        }
        break;

    case IIC_IT_MODE:
        if (!iic_wait_idle(iic)) {
            return;
        }
        iic_set_transfer_context(iic, data, size, false, false, seq_mode);
        iic_start_transfer(iic, false, size, seq_mode);
        break;

    case IIC_DMA_MODE:
        if (!iic_wait_idle(iic)) {
            return;
        }
        if (!iic_prepare_dma(iic, data, size, false)) {
            return;
        }

        iic_set_transfer_context(iic, data, size, false, true, seq_mode);
        iic_start_transfer(iic, false, size, seq_mode);
        break;

    default:
        break;
    }
}

/**
 * @brief 访问I2C从机寄存器(阻塞)
 * @param iic iic实例
 * @param mem_addr 从机内部地址
 * @param data 数据缓冲区
 * @param size 数据长度
 * @param mem_mode 读写模式
 * @param mem8bit_flag 地址宽度标志(1:8bit, 0:16bit)
 */
void IICAccessMem(IICInstance *iic, uint16_t mem_addr, uint8_t *data,
    uint16_t size, IIC_Mem_Mode_e mem_mode, uint8_t mem8bit_flag)
{
    uint8_t addr_buf[2];
    uint8_t addr_len;
    uint16_t i;

    if ((iic == NULL) || (data == NULL) || (size == 0U) || (iic->transfer_busy)) {
        return;
    }
    if (!iic_wait_idle(iic)) {
        return;
    }

    addr_len = (mem8bit_flag != 0U) ? 1U : 2U;
    addr_buf[0] = (uint8_t) ((mem_addr >> 8) & 0xFFU);
    addr_buf[1] = (uint8_t) (mem_addr & 0xFFU);

    if (mem_mode == IIC_WRITE_MEM) {
        DL_I2C_startControllerTransfer(iic->handle, iic->dev_address,
            DL_I2C_CONTROLLER_DIRECTION_TX, (uint16_t) (addr_len + size));

        for (i = 0; i < addr_len; i++) {
            uint8_t addr_byte = addr_buf[(uint8_t) (2U - addr_len + i)];
            while (DL_I2C_isControllerTXFIFOFull(iic->handle)) {
            }
            DL_I2C_transmitControllerData(iic->handle, addr_byte);
        }

        for (i = 0; i < size; i++) {
            while (DL_I2C_isControllerTXFIFOFull(iic->handle)) {
            }
            DL_I2C_transmitControllerData(iic->handle, data[i]);
        }

        (void) iic_wait_bus_free(iic);
        (void) iic_wait_idle(iic);
        return;
    }

    if (mem_mode == IIC_READ_MEM) {
        DL_I2C_startControllerTransfer(iic->handle, iic->dev_address,
            DL_I2C_CONTROLLER_DIRECTION_TX, addr_len);

        for (i = 0; i < addr_len; i++) {
            uint8_t addr_byte = addr_buf[(uint8_t) (2U - addr_len + i)];
            while (DL_I2C_isControllerTXFIFOFull(iic->handle)) {
            }
            DL_I2C_transmitControllerData(iic->handle, addr_byte);
        }

        if (!iic_wait_bus_free(iic) || !iic_wait_idle(iic)) {
            return;
        }

        DL_I2C_startControllerTransfer(iic->handle, iic->dev_address,
            DL_I2C_CONTROLLER_DIRECTION_RX, size);

        for (i = 0; i < size; i++) {
            while (DL_I2C_isControllerRXFIFOEmpty(iic->handle)) {
            }
            data[i] = DL_I2C_receiveControllerData(iic->handle);
        }

        (void) iic_wait_idle(iic);
    }
}

/**
 * @brief I2C中断分发函数
 * @param hi2c I2C外设基地址
 */
void BSPI2C_IRQHandler(I2C_HandleTypeDef *hi2c)
{
    IICInstance *instance = iic_find_instance_by_handle(hi2c);
    DL_I2C_IIDX iidx;

    if (instance == NULL) {
        return;
    }

    while ((iidx = DL_I2C_getPendingInterrupt(hi2c)) != DL_I2C_IIDX_NO_INT) {
        switch (iidx) {
            case DL_I2C_IIDX_CONTROLLER_TXFIFO_TRIGGER:
            case DL_I2C_IIDX_CONTROLLER_TXFIFO_EMPTY:
                if ((instance->use_dma == false) && (instance->tx_buffer != NULL)) {
                    iic_feed_tx_fifo(instance);
                }
                break;

            case DL_I2C_IIDX_CONTROLLER_RXFIFO_TRIGGER:
            case DL_I2C_IIDX_CONTROLLER_RXFIFO_FULL:
                if ((instance->use_dma == false) && (instance->rx_buffer != NULL)) {
                    iic_drain_rx_fifo(instance);
                }
                break;

            case DL_I2C_IIDX_CONTROLLER_TX_DONE:
            case DL_I2C_IIDX_CONTROLLER_EVENT1_DMA_DONE:
                if (instance->tx_buffer != NULL) {
                    iic_finalize_success(instance);
                }
                break;

            case DL_I2C_IIDX_CONTROLLER_RX_DONE:
            case DL_I2C_IIDX_CONTROLLER_EVENT2_DMA_DONE:
                if (instance->rx_buffer != NULL) {
                    iic_drain_rx_fifo(instance);
                    iic_finalize_success(instance);
                }
                break;

            default:
                break;
        }
    }
}

void I2C0_IRQHandler(void)
{
    BSPI2C_IRQHandler(I2C0);
}

void I2C1_IRQHandler(void)
{
    BSPI2C_IRQHandler(I2C1);
}
