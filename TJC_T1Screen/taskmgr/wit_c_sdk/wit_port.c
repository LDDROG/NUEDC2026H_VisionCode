/**
 * @file    wit_port.c
 * @brief   WIT 传感器 BSP 移植层实现
 * @author  neozng
 * @date    2025-XX-XX
 *
 * 通过项目 BSP USART 层对接 WIT SDK，实现与 WIT 传感器的 UART 通信。
 *
 * 注意: 不额外分配 RX 缓冲区，直接复用 USARTRegister() 时用户传入的 buffer。
 *       用户需确保 buffer 大小 >= WIT_DATA_BUFF_SIZE (256) 以容纳完整帧。
 *
 * == 数据流 ==
 *
 * RX 路径:
 *   WIT 传感器 --UART--> BSP USART (DMA) --> usart->recv_buff
 *       |                                          |
 *       |                             USARTServiceTickUs() 判帧
 *       |                                          |
 *       |                              WitPortOnFrameReady()
 *       |                                          |
 *                        逐字节调用 WitSerialDataIn(byte)
 *                                      |
 *                              WIT SDK 内部解析器
 *                           (0x55 帧 / Modbus CRC)
 *                                      |
 *                          sReg[] 更新 + RegUpdateCb
 *                                      |
 *                          用户 data_callback(reg, num)
 *
 * TX 路径:
 *   WitWriteReg() --> p_WitSerialWriteFunc()
 *                          |
 *                   WitPortSerialWrite()
 *                          |
 *              USARTSend(..., BLOCKING) --> UART0 TX --> WIT 传感器
 */

#include "wit_port.h"
#include "bsp_time.h"

/* ========== 内部变量 ========== */

/** 当前占用的 USART 实例 (NULL = 空闲) */
static USARTInstance *g_wit_usart = NULL;

/* ========== 内部回调实现 ========== */

/**
 * @brief WIT SDK SerialWrite 回调实现
 *
 * 通过 BSP USART 以阻塞模式发送数据。
 * 配置命令通常 < 10 字节，阻塞发送耗时微秒级，简单可靠。
 *
 * @param data   待发送数据缓冲区
 * @param len    数据长度 (字节)
 */
static void WitPortSerialWrite(uint8_t *data, uint32_t len)
{
    if ((g_wit_usart == NULL) || (data == NULL) || (len == 0U))
    {
        return;
    }
    USARTSend(g_wit_usart, data, (uint16_t)len, USART_TRANSFER_BLOCKING);
}

/**
 * @brief WIT SDK DelayMs 回调实现
 *
 * 使用 bsp_time 库的阻塞毫秒延时。
 *
 * @param ms  延时毫秒数
 */
static void WitPortDelayMs(uint16_t ms)
{
    TimeDelayMs(ms);
}

/**
 * @brief BSP USART 帧接收完成回调
 *
 * 由 USARTFinishFrame() 在 ISR 上下文中调用。
 * 将 BSP 收到的一帧数据逐字节喂入 WIT SDK 解析器。
 *
 * WIT SDK 的 WitSerialDataIn() 自带帧同步 (0x55 头 / Modbus CRC)，
 * BSP 层只需提供"一组新收到的字节"，无需关心 WIT 帧边界。
 */
static void WitPortOnFrameReady(void)
{
    uint16_t size;
    uint16_t i;

    if (g_wit_usart == NULL)
    {
        return;
    }

    size = g_wit_usart->last_rx_size;
    g_wit_usart->last_rx_size = 0U; /* 消费, 避免下次重复解析 */

    if (size > g_wit_usart->recv_buff_size)
    {
        size = g_wit_usart->recv_buff_size;
    }

    if (size == 0U)
    {
        return;
    }

    /* 逐字节喂入 WIT SDK 解析器 */
    for (i = 0U; i < size; i++)
    {
        WitSerialDataIn(g_wit_usart->recv_buff[i]);
    }
}

/* ========== 公开 API ========== */

int32_t WitPortInit(WitPortInitConfig_s *config)
{
    if (config == NULL)
    {
        return WIT_HAL_INVAL;
    }

    if (config->usart == NULL)
    {
        return WIT_HAL_INVAL;
    }

    if (config->protocol > WIT_PROTOCOL_905x_MODBUS)
    {
        return WIT_HAL_INVAL;
    }

    /* 单实例: 不允许重复初始化 */
    if (g_wit_usart != NULL)
    {
        return WIT_HAL_BUSY;
    }

    /* 1. 注册 WIT SDK 回调 */
    if (WitSerialWriteRegister(WitPortSerialWrite) != WIT_HAL_OK)
    {
        return WIT_HAL_EMPTY;
    }
    if (WitDelayMsRegister(WitPortDelayMs) != WIT_HAL_OK)
    {
        return WIT_HAL_EMPTY;
    }
    if (WitRegisterCallBack(config->data_callback) != WIT_HAL_OK)
    {
        return WIT_HAL_EMPTY;
    }

    /* 2. 初始化 WIT SDK 协议和地址 */
    if (WitInit(config->protocol, config->addr) != WIT_HAL_OK)
    {
        return WIT_HAL_ERROR;
    }

    /* 3. 接管 USART 回调: 保留用户原有的 recv_buff, 只替换帧回调 */
    config->usart->module_callback = WitPortOnFrameReady;

    /* 4. 用新配置重启 USART 接收 */
    USARTServiceInit(config->usart);

    /* 5. 保存实例指针 */
    g_wit_usart = config->usart;

    return WIT_HAL_OK;
}

void WitPortDeInit(void)
{
    WitDeInit();
    g_wit_usart = NULL;
}
