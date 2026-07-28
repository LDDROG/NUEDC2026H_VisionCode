#include "bsp_spi.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h> 
#include "bsp_time.h"
typedef enum
{
    SPI_TRANSFER_NONE = 0,
    SPI_TRANSFER_TX_DMA,
    SPI_TRANSFER_RX_DMA,
    SPI_TRANSFER_TXRX_DMA,
    SPI_TRANSFER_IT,
} SPI_TRANSFER_STATE_e;

/* 所有的spi instance保存于此,用于callback时判断中断来源*/
static SPIInstance *spi_instance[SPI_DEVICE_CNT] = {NULL};
static uint8_t idx = 0;                         // 配合中断以及初始化
uint8_t SPIDeviceOnGoing[SPI_DEVICE_CNT] = {1U, 1U}; // 用于判断当前spi是否正在传输,防止多个模块同时使用一个spi总线 (0: 正在传输, 1: 未传输)

/* SPI主机纯接收时用于持续打时钟的dummy字节 */
static uint8_t s_spi_dma_dummy_tx = 0xFFU;
/* IT模式下每次最多向TX FIFO补充的字节数 */
#define SPI_IT_TX_BURST_MAX 3U

/**
 * @brief 进入不可恢复错误处理
 */
static void BSP_SPI_Fatal(void)
{
    while (1)
    {
        ;
    }
}


/**
 * @brief 根据SPI实例地址获取总线占用状态指针
 * @param spi SPI寄存器基址
 * @return uint8_t* 对应总线状态变量地址,不支持的SPI返回NULL
 */
static uint8_t *BSP_SPI_GetBusStatePtr(SPI_HandleTypeDef *spi)
{
    if (spi == SPI0)
    {
        return &SPIDeviceOnGoing[0];
    }
    if (spi == SPI1)
    {
        return &SPIDeviceOnGoing[1];
    }

    return NULL;
}

/**
 * @brief 拉低CS并更新软件状态
 * @param ins SPI实例
 */
static void BSP_SPI_Select(SPIInstance *ins)
{
    DL_GPIO_clearPins(ins->GPIOx, ins->cs_pin);
    *ins->cs_pin_state = 0U;
    ins->CS_State = 0U;
}

/**
 * @brief 拉高CS并更新软件状态
 * @param ins SPI实例
 */
static void BSP_SPI_Deselect(SPIInstance *ins)
{
    DL_GPIO_setPins(ins->GPIOx, ins->cs_pin);
    *ins->cs_pin_state = 1U;
    ins->CS_State = 1U;
}

/**
 * @brief 阻塞方式执行SPI收发
 * @param ins SPI实例
 * @param rx 接收缓冲区,为NULL表示忽略接收数据
 * @param tx 发送缓冲区,为NULL表示发送0xFF
 * @param len 传输长度(字节)
 */
static void BSP_SPI_BlockingTransfer(SPIInstance *ins, uint8_t *rx, uint8_t *tx, uint8_t len)
{
    uint8_t tx_count;
    uint8_t rx_count;
    uint8_t rx_byte;
    uint8_t chunk;
    
    if ((ins == NULL) || (len == 0U))
    {
        return;
    }


    BSP_SPI_Select(ins);

    if (rx == NULL)
    {
        tx_count = 0U;
        while (tx_count < len)
        {
            uint8_t out = (tx != NULL) ? tx[tx_count] : 0xFFU;
            
            if (!DL_SPI_transmitDataCheck8(ins->spi_handle, out))
            {
                continue;
            }
            tx_count++;
        }

        while (DL_SPI_isBusy(ins->spi_handle))
        {
            ;
        }

        BSP_SPI_Deselect(ins);
        ins->transfer_mode = SPI_TRANSFER_NONE;
        return;
    }
    /* 清掉历史残留，避免上次传输遗留数据干扰本次逻辑 */
    while (!DL_SPI_isRXFIFOEmpty(ins->spi_handle))
    {
        (void)DL_SPI_receiveData8(ins->spi_handle);
    }

    tx_count = 0U;
    rx_count = 0U;
    while (rx_count < len)
    {
        /* 每轮最多写3字节，避免RX FIFO积压 */
        chunk = 0U;
        while ((tx_count < len) && (chunk < 4U))
        {
            uint8_t out = (tx != NULL) ? tx[tx_count] : 0xFFU;

            if (!DL_SPI_transmitDataCheck8(ins->spi_handle, out))
            {
                break;
            }
            tx_count++;     
            chunk++;
        }
        /* 每轮最多读3字节，和写节奏保持一致 */
        chunk = 0U;
        while ((rx_count < tx_count) && (chunk < 4U) &&
               DL_SPI_receiveDataCheck8(ins->spi_handle, &rx_byte))
        {
            if (rx != NULL)
            {
                rx[rx_count] = rx_byte;
            }
            rx_count++;
            chunk++;
        }
    }
    while (DL_SPI_isBusy(ins->spi_handle))
    {
        ;
    }

    BSP_SPI_Deselect(ins);
    ins->transfer_mode = SPI_TRANSFER_NONE;
}
/**
 * @brief 根据SPI句柄查找已注册实例
 * @param hspi SPI寄存器基址
 * @return SPIInstance* 匹配到的实例指针,未找到返回NULL
 */
static SPIInstance *BSP_SPI_FindInstanceByHandle(SPI_HandleTypeDef *hspi)
{
    uint8_t i;

    for (i = 0U; i < idx; i++)
    {
        if ((spi_instance[i] != NULL) && (spi_instance[i]->spi_handle == hspi))
        {
            return spi_instance[i];
        }
    }
    return NULL;
}

/**
 * @brief 根据SPI句柄查找当前活跃实例(优先CS为低的实例)
 * @param hspi SPI寄存器基址
 * @return SPIInstance* 活跃实例或匹配句柄的首个实例
 */
static SPIInstance *BSP_SPI_FindActiveInstanceByHandle(SPI_HandleTypeDef *hspi)
{
    SPIInstance *fallback = NULL;
    uint8_t i;

    for (i = 0U; i < idx; i++)
    {
        SPIInstance *ins = spi_instance[i];

        if ((ins == NULL) || (ins->spi_handle != hspi))
        {
            continue;
        }

        if (fallback == NULL)
        {
            fallback = ins;
        }

        if (ins->CS_State == 0U)
        {
            return ins;
        }
    }

    return fallback;
}

/**
 * @brief 收尾异步传输:关中断/事件、等待总线空闲、释放CS并回调
 * @param ins SPI实例
 */
static void BSP_SPI_FinishAsyncTransfer(SPIInstance *ins)
{
    if (ins == NULL)
    {
        return;
    }

        DL_SPI_disableInterrupt(ins->spi_handle,
                                DL_SPI_INTERRUPT_DMA_DONE_TX |
                                    DL_SPI_INTERRUPT_DMA_DONE_RX |
                                    DL_SPI_INTERRUPT_TX_EMPTY |
                                    DL_SPI_INTERRUPT_RX);
    DL_SPI_disableDMAReceiveEvent(
        ins->spi_handle, DL_SPI_DMA_INTERRUPT_RX | DL_SPI_DMA_INTERRUPT_RX_TIMEOUT);
    DL_SPI_disableDMATransmitEvent(ins->spi_handle);

    while (DL_SPI_isBusy(ins->spi_handle))
    {
        ;
    }

    BSP_SPI_Deselect(ins);
    ins->transfer_mode = SPI_TRANSFER_NONE;
    ins->it_active = 0U;

    if (ins->callback != NULL)
    {
        ins->callback(ins);
    }
}

/**
 * @brief 启动一次SPI中断模式传输
 * @param ins SPI实例
 * @param rx 接收缓冲区,NULL表示丢弃接收数据
 * @param tx 发送缓冲区,NULL表示发送0xFF
 * @param len 传输长度
 */
static void BSP_SPI_StartITTransfer(SPIInstance *ins, uint8_t *rx, uint8_t *tx, uint8_t len)
{
    uint8_t tx_byte;
    uint8_t burst;

    if ((ins == NULL) || (len == 0U))
    {
        return;
    }

    ins->it_tx_buf = tx;
    ins->it_rx_buf = rx;
    ins->it_length = len;
    ins->it_tx_count = 0U;
    ins->it_rx_count = 0U;
    ins->it_active = 1U;

    while (!DL_SPI_isRXFIFOEmpty(ins->spi_handle))
    {
        (void)DL_SPI_receiveData8(ins->spi_handle);
    }

    ins->transfer_mode = SPI_TRANSFER_IT;
    BSP_SPI_Select(ins);

    DL_SPI_disableDMATransmitEvent(ins->spi_handle);
    DL_SPI_disableDMAReceiveEvent(
        ins->spi_handle, DL_SPI_DMA_INTERRUPT_RX | DL_SPI_DMA_INTERRUPT_RX_TIMEOUT);

    DL_SPI_disableInterrupt(ins->spi_handle,
                            DL_SPI_INTERRUPT_TX |
                                DL_SPI_INTERRUPT_TX_EMPTY |
                                DL_SPI_INTERRUPT_RX |
                                DL_SPI_INTERRUPT_DMA_DONE_TX |
                                DL_SPI_INTERRUPT_DMA_DONE_RX);
    DL_SPI_clearInterruptStatus(ins->spi_handle,
                                DL_SPI_INTERRUPT_TX |
                                    DL_SPI_INTERRUPT_TX_EMPTY |
                                    DL_SPI_INTERRUPT_RX);
    DL_SPI_enableInterrupt(ins->spi_handle, DL_SPI_INTERRUPT_TX_EMPTY);
    /* 首次最多填3字节，后续由TX_EMPTY中断继续推进 */
    burst = 0U;
    while ((ins->it_tx_count < ins->it_length) && (burst < SPI_IT_TX_BURST_MAX))
        {
        tx_byte = (ins->it_tx_buf != NULL) ? ins->it_tx_buf[ins->it_tx_count] : 0xFFU;
        if (!DL_SPI_transmitDataCheck8(ins->spi_handle, tx_byte))
        {
            break;
        }
        ins->it_tx_count++;
        burst++;
    }

    
}

/**
 * @brief SPI中断统一分发处理
 * @param spi SPI寄存器基址
 */
static void BSPSPIHandleIRQ(SPI_Regs *spi)
{
    DL_SPI_IIDX iidx;
    SPIInstance *ins = BSP_SPI_FindActiveInstanceByHandle(spi);

    if (ins == NULL)
    {
        return;
    }

    iidx = DL_SPI_getPendingInterrupt(spi);
    while ((uint32_t)iidx != 0U)
    {
        switch (iidx)
        {
        case DL_SPI_IIDX_DMA_DONE_TX:
            if (ins->transfer_mode == SPI_TRANSFER_TX_DMA)
            {
                BSP_SPI_FinishAsyncTransfer(ins);
            }
            break;
   
        case DL_SPI_IIDX_DMA_DONE_RX:
            if ((ins->transfer_mode == SPI_TRANSFER_RX_DMA) ||
                (ins->transfer_mode == SPI_TRANSFER_TXRX_DMA))
            {
                BSP_SPI_FinishAsyncTransfer(ins);
            }
            break;

        case DL_SPI_IIDX_TX_EMPTY:
            if ((ins->transfer_mode == SPI_TRANSFER_IT) && (ins->it_active != 0U))
            {
                uint8_t rx_byte;
                uint8_t burst;

                /* RX依赖TX时钟，统一在TX_EMPTY时机回收RX FIFO */
                while (DL_SPI_receiveDataCheck8(ins->spi_handle, &rx_byte))
                {
                    if ((ins->it_rx_buf != NULL) && (ins->it_rx_count < ins->it_length))
                    {
                        ins->it_rx_buf[ins->it_rx_count] = rx_byte;
                    }
                    if (ins->it_rx_count < ins->it_length)
                    {
                        ins->it_rx_count++;
                    }
                }

                burst = 0U;
                while ((ins->it_tx_count < ins->it_length) && (burst < SPI_IT_TX_BURST_MAX))
                {
                    uint8_t tx_byte = (ins->it_tx_buf != NULL) ? ins->it_tx_buf[ins->it_tx_count]
                                                               : 0xFFU;

                    if (!DL_SPI_transmitDataCheck8(ins->spi_handle, tx_byte))
                    {
                        break;
                    }
                    ins->it_tx_count++;
                    burst++;
                }

                while (DL_SPI_receiveDataCheck8(ins->spi_handle, &rx_byte))
                {
                    if ((ins->it_rx_buf != NULL) && (ins->it_rx_count < ins->it_length))
                    {
                        ins->it_rx_buf[ins->it_rx_count] = rx_byte;
                    }
                    if (ins->it_rx_count < ins->it_length)
                    {
                        ins->it_rx_count++;
                    }
                }

                if ((ins->it_tx_count >= ins->it_length) &&
                    (ins->it_rx_count >= ins->it_length) &&
                    DL_SPI_isTXFIFOEmpty(ins->spi_handle) && !DL_SPI_isBusy(ins->spi_handle))
                {
                    BSP_SPI_FinishAsyncTransfer(ins);
                }
            }
            break;

        default:
            break;
        }

        iidx = DL_SPI_getPendingInterrupt(spi);
    }
}

/**
 * @brief 注册一个SPI设备实例并初始化运行时状态
 * @param conf SPI初始化参数
 * @return SPIInstance* 注册成功返回实例指针,失败返回NULL
 */
SPIInstance *SPIRegister(SPI_Init_Config_s *conf)
{
    SPIInstance *instance;
    uint8_t *bus_state;

    if (conf == NULL)
    {
        return NULL;
    }

    if (idx >= MX_SPI_BUS_SLAVE_CNT) // 超过最大实例数
    {
        BSP_SPI_Fatal();
    }
    
    instance = (SPIInstance *)malloc(sizeof(SPIInstance));
    if (instance == NULL)
    {
        return NULL;
    }
    memset(instance, 0, sizeof(SPIInstance));

    instance->spi_handle = conf->spi_handle;
    instance->GPIOx = conf->GPIOx;
    instance->cs_pin = conf->cs_pin;
    instance->spi_work_mode = conf->spi_work_mode;
    instance->callback = conf->callback;
    instance->id = conf->id;

    instance->dma_rx_channel = conf->dma_rx_channel;
    instance->dma_tx_channel = conf->dma_tx_channel;

    bus_state = BSP_SPI_GetBusStatePtr(instance->spi_handle);
    if (bus_state == NULL)
    {
        BSP_SPI_Fatal();
    }

    /* 注册后先释放片选，确保总线处于空闲高电平 */
    DL_GPIO_setPins(instance->GPIOx, instance->cs_pin);
    instance->cs_pin_state = bus_state;
    *instance->cs_pin_state = 1U;
    instance->CS_State = *instance->cs_pin_state;

    if (instance->spi_handle == SPI0)
    {
        NVIC_ClearPendingIRQ(SPI0_INT_IRQn);
        NVIC_EnableIRQ(SPI0_INT_IRQn);
    }
    else if (instance->spi_handle == SPI1)
    {
        NVIC_ClearPendingIRQ(SPI1_INT_IRQn);
        NVIC_EnableIRQ(SPI1_INT_IRQn);
    }

    spi_instance[idx++] = instance;
    return instance;
}

/**
 * @brief 发送一帧SPI数据
 * @param spi_ins SPI实例
 * @param ptr_data 待发送数据地址
 * @param len 发送长度(字节)
 */
void SPITransmit(SPIInstance *spi_ins, uint8_t *ptr_data, uint8_t len)
{
    if ((spi_ins == NULL) || (ptr_data == NULL) || (len == 0U))
    {
        return;
    }

    while (*spi_ins->cs_pin_state == 0U)
    {
        ;
    }

    switch (spi_ins->spi_work_mode)
    {
    case SPI_DMA_MODE:
        if (spi_ins->dma_tx_channel == SPI_DMA_CHANNEL_NONE)
        {
            BSP_SPI_BlockingTransfer(spi_ins, NULL, ptr_data, len);
            break;
        }
        spi_ins->transfer_mode = SPI_TRANSFER_TX_DMA;

        BSP_SPI_Select(spi_ins);

        DL_DMA_disableChannel(DMA, spi_ins->dma_tx_channel);
        DL_DMA_setSrcAddr(DMA, spi_ins->dma_tx_channel, (uint32_t)(uintptr_t)ptr_data);
        DL_DMA_setDestAddr(
            DMA, spi_ins->dma_tx_channel, (uint32_t)(uintptr_t)&(spi_ins->spi_handle->TXDATA));
        DL_DMA_setTransferSize(DMA, spi_ins->dma_tx_channel, len);
        /* TX: block -> fix */
        DL_DMA_setSrcIncrement(DMA, spi_ins->dma_tx_channel, DL_DMA_ADDR_INCREMENT);
        DL_DMA_setDestIncrement(DMA, spi_ins->dma_tx_channel, DL_DMA_ADDR_UNCHANGED);

        DL_SPI_enableDMATransmitEvent(spi_ins->spi_handle);
        DL_SPI_enableInterrupt(spi_ins->spi_handle, DL_SPI_INTERRUPT_DMA_DONE_TX);
        DL_DMA_enableChannel(DMA, spi_ins->dma_tx_channel);
        break;
    case SPI_IT_MODE:
        BSP_SPI_StartITTransfer(spi_ins, NULL, ptr_data, len);
        break;
    case SPI_BLOCK_MODE:
        BSP_SPI_BlockingTransfer(spi_ins, NULL, ptr_data, len);
        break;
    default:
        BSP_SPI_Fatal(); // error mode! 请查看是否正确设置模式，或出现指针越界导致模式被异常修改的情况
        break;
    }
}

/**
 * @brief 接收一帧SPI数据
 * @param spi_ins SPI实例
 * @param ptr_data 接收缓冲区地址
 * @param len 接收长度(字节)
 */
void SPIRecv(SPIInstance *spi_ins, uint8_t *ptr_data, uint8_t len)
{
    if ((spi_ins == NULL) || (ptr_data == NULL) || (len == 0U))
    {
        return;
    }

    while (*spi_ins->cs_pin_state == 0U)
    {
        ;
    }

    switch (spi_ins->spi_work_mode)
    {
    case SPI_DMA_MODE:
        if ((spi_ins->dma_rx_channel == SPI_DMA_CHANNEL_NONE) ||
            (spi_ins->dma_tx_channel == SPI_DMA_CHANNEL_NONE))
        {
            BSP_SPI_BlockingTransfer(spi_ins, ptr_data, NULL, len);
            break;
        }

        spi_ins->transfer_mode = SPI_TRANSFER_RX_DMA;
        BSP_SPI_Select(spi_ins);

        DL_DMA_disableChannel(DMA, spi_ins->dma_rx_channel);
        DL_DMA_setSrcAddr(
            DMA, spi_ins->dma_rx_channel, (uint32_t)(uintptr_t)&(spi_ins->spi_handle->RXDATA));
        DL_DMA_setDestAddr(DMA, spi_ins->dma_rx_channel, (uint32_t)(uintptr_t)ptr_data);
        DL_DMA_setTransferSize(DMA, spi_ins->dma_rx_channel, len);
        /* RX: fix -> block */
        DL_DMA_setSrcIncrement(DMA, spi_ins->dma_rx_channel, DL_DMA_ADDR_UNCHANGED);
        DL_DMA_setDestIncrement(DMA, spi_ins->dma_rx_channel, DL_DMA_ADDR_INCREMENT);

        DL_DMA_disableChannel(DMA, spi_ins->dma_tx_channel);
        DL_DMA_setSrcAddr(
            DMA, spi_ins->dma_tx_channel, (uint32_t)(uintptr_t)&s_spi_dma_dummy_tx);
        DL_DMA_setDestAddr(
            DMA, spi_ins->dma_tx_channel, (uint32_t)(uintptr_t)&(spi_ins->spi_handle->TXDATA));
        DL_DMA_setTransferSize(DMA, spi_ins->dma_tx_channel, len);
        /* 为纯接收提供时钟: TX fix -> fix */
        DL_DMA_setSrcIncrement(DMA, spi_ins->dma_tx_channel, DL_DMA_ADDR_UNCHANGED);
        DL_DMA_setDestIncrement(DMA, spi_ins->dma_tx_channel, DL_DMA_ADDR_UNCHANGED);

        DL_SPI_enableDMAReceiveEvent(spi_ins->spi_handle, DL_SPI_DMA_INTERRUPT_RX);
        DL_SPI_enableDMATransmitEvent(spi_ins->spi_handle);
        DL_SPI_enableInterrupt(spi_ins->spi_handle, DL_SPI_INTERRUPT_DMA_DONE_RX);

        DL_DMA_enableChannel(DMA, spi_ins->dma_rx_channel);
        DL_DMA_enableChannel(DMA, spi_ins->dma_tx_channel);
        break;
    case SPI_IT_MODE:
        BSP_SPI_StartITTransfer(spi_ins, ptr_data, NULL, len);
        break;
    case SPI_BLOCK_MODE:
        BSP_SPI_BlockingTransfer(spi_ins, ptr_data, NULL, len);
        break;
    default:
        BSP_SPI_Fatal(); // error mode! 请查看是否正确设置模式，或出现指针越界导致模式被异常修改的情况
        break;
    }
}

/**
 * @brief 同时发送并接收一帧SPI数据
 * @param spi_ins SPI实例
 * @param ptr_data_rx 接收缓冲区地址
 * @param ptr_data_tx 发送缓冲区地址
 * @param len 传输长度(字节)
 */
void SPITransRecv(SPIInstance *spi_ins, uint8_t *ptr_data_rx, uint8_t *ptr_data_tx, uint8_t len)
{
    if ((spi_ins == NULL) || (ptr_data_rx == NULL) || (ptr_data_tx == NULL) || (len == 0U))
    {
        return;
    }

    while (*spi_ins->cs_pin_state == 0U)
    {
        ;
    }

    switch (spi_ins->spi_work_mode)
    {
    case SPI_DMA_MODE:
        if ((spi_ins->dma_rx_channel == SPI_DMA_CHANNEL_NONE) ||
            (spi_ins->dma_tx_channel == SPI_DMA_CHANNEL_NONE))
        {
            BSP_SPI_BlockingTransfer(spi_ins, ptr_data_rx, ptr_data_tx, len);
            break;
        }
        spi_ins->transfer_mode = SPI_TRANSFER_TXRX_DMA;

        BSP_SPI_Select(spi_ins);

        DL_DMA_disableChannel(DMA, spi_ins->dma_rx_channel);
        DL_DMA_setSrcAddr(
            DMA, spi_ins->dma_rx_channel, (uint32_t)(uintptr_t)&(spi_ins->spi_handle->RXDATA));
        DL_DMA_setDestAddr(DMA, spi_ins->dma_rx_channel, (uint32_t)(uintptr_t)ptr_data_rx);
        DL_DMA_setTransferSize(DMA, spi_ins->dma_rx_channel, len);
        /* RX: fix -> block */
        DL_DMA_setSrcIncrement(DMA, spi_ins->dma_rx_channel, DL_DMA_ADDR_UNCHANGED);
        DL_DMA_setDestIncrement(DMA, spi_ins->dma_rx_channel, DL_DMA_ADDR_INCREMENT);

        DL_DMA_disableChannel(DMA, spi_ins->dma_tx_channel);
        DL_DMA_setSrcAddr(DMA, spi_ins->dma_tx_channel, (uint32_t)(uintptr_t)ptr_data_tx);
        DL_DMA_setDestAddr(
            DMA, spi_ins->dma_tx_channel, (uint32_t)(uintptr_t)&(spi_ins->spi_handle->TXDATA));
        DL_DMA_setTransferSize(DMA, spi_ins->dma_tx_channel, len);
        /* TX: block -> fix */
        DL_DMA_setSrcIncrement(DMA, spi_ins->dma_tx_channel, DL_DMA_ADDR_INCREMENT);
        DL_DMA_setDestIncrement(DMA, spi_ins->dma_tx_channel, DL_DMA_ADDR_UNCHANGED);

        DL_SPI_enableDMAReceiveEvent(spi_ins->spi_handle, DL_SPI_DMA_INTERRUPT_RX);
        DL_SPI_enableDMATransmitEvent(spi_ins->spi_handle);
        DL_SPI_enableInterrupt(spi_ins->spi_handle, DL_SPI_INTERRUPT_DMA_DONE_RX);

        DL_DMA_enableChannel(DMA, spi_ins->dma_rx_channel);
        DL_DMA_enableChannel(DMA, spi_ins->dma_tx_channel);
        break;
    case SPI_IT_MODE:
        BSP_SPI_StartITTransfer(spi_ins, ptr_data_rx, ptr_data_tx, len);
        break;
    case SPI_BLOCK_MODE:
        BSP_SPI_BlockingTransfer(spi_ins, ptr_data_rx, ptr_data_tx, len);
        break;
    default:
        BSP_SPI_Fatal(); // error mode! 请查看是否正确设置模式，或出现指针越界导致模式被异常修改的情况
        break;
    }
}

/**
 * @brief 设置SPI实例收发模式
 * @param spi_ins SPI实例
 * @param spi_mode 工作模式
 */
void SPISetMode(SPIInstance *spi_ins, SPI_TXRX_MODE_e spi_mode)
{
    if (spi_ins == NULL)
    {
        return;
    }

    if (spi_mode != SPI_DMA_MODE && spi_mode != SPI_IT_MODE && spi_mode != SPI_BLOCK_MODE)
    {
        BSP_SPI_Fatal(); // error mode! 请查看是否正确设置模式，或出现指针越界导致模式被异常修改的情况
    }

    if (spi_ins->spi_work_mode != spi_mode)
    {
        spi_ins->spi_work_mode = spi_mode;
    }
}

/**
 * @brief 当SPI接收完成,将会调用此回调函数,可以进行协议解析或其他必须的数据处理等
 *
 * @param hspi spi handle
 */
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    SPIInstance *ins = BSP_SPI_FindInstanceByHandle(hspi);

    if (ins != NULL)
    {
        BSP_SPI_FinishAsyncTransfer(ins);
    }
}

/**
 * @brief 和RxCpltCallback共用解析即可,这里只是形式上封装一下,不用重复写
 *        这是对HAL库的__weak函数的重写,传输使用IT或DMA模式,在传输完成时会调用此函数
 * @param hspi spi handle
 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    HAL_SPI_RxCpltCallback(hspi); // 直接调用接收完成的回调函数
}

/**
 * @brief SPI0中断服务函数,用于处理DMA完成及发送FIFO空事件
 */
void SPI0_IRQHandler(void)
{
    BSPSPIHandleIRQ(SPI0);
}

void SPI1_IRQHandler(void)
{
    BSPSPIHandleIRQ(SPI1);
}
