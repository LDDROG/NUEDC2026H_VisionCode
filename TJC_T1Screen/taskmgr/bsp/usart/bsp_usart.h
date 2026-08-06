#ifndef BSP_RC_H
#define BSP_RC_H

#include <stdint.h>
#include "ti_msp_dl_config.h"

#define DEVICE_USART_CNT 4     // MC02串口数量4个，其中
#define USART_RXBUFF_LIMIT 1024 // 单实例接收缓冲区上限
#define USART_DMA_CHANNEL_NONE 0xFFU
#define USART_RX_FRAME_GAP_US_DEFAULT 2000U

/* STM32 HAL compatible type aliases for API stability */
typedef UART_Regs UART_HandleTypeDef;

// 模块回调函数,用于解析协议
typedef void (*usart_module_callback)();

/* 发送模式枚举 */
typedef enum
{
    USART_TRANSFER_NONE=0,
    USART_TRANSFER_BLOCKING,
    USART_TRANSFER_IT,
    USART_TRANSFER_DMA,
} USART_TRANSFER_MODE;

// 串口实例结构体,每个module都要包含一个实例.
// 由于串口是独占的点对点通信,所以不需要考虑多个module同时使用一个串口的情况,因此不用加入id;当然也可以选择加入,这样在bsp层可以访问到module的其他信息
typedef struct
{
    uint8_t *recv_buff;                    // 外部提供的静态接收buffer首地址
    uint16_t recv_buff_size;               // 本次DMA接收窗口大小
    UART_HandleTypeDef *usart_handle;      // 实例对应的usart_handle
    usart_module_callback module_callback; // 解析收到的数据的回调函数

    /* MSPM0 DMA runtime configuration */
    uint8_t dma_rx_channel;
    uint8_t dma_tx_channel;

    /* Internal state, module users normally don't need these fields */
    uint8_t is_rx_active;
    uint32_t stable_ticks_us;
    uint16_t last_rx_progress;
    uint16_t it_rx_count;               // IT接收模式下当前累计接收长度
    uint32_t rx_frame_gap_us;
    uint16_t last_rx_size;                // 最近一次判帧得到的有效字节数

    const uint8_t *tx_it_buf;            // IT发送模式的零拷贝发送源
    uint16_t tx_it_size;
    uint16_t tx_it_index;
    uint8_t tx_it_busy;
} USARTInstance;

/* usart 初始化配置结构体 */
typedef struct
{
    uint8_t *recv_buff;                    // 外部提供的静态接收buffer首地址
    uint16_t recv_buff_size;               // 模块接收一包数据的大小
    UART_HandleTypeDef *usart_handle;      // 实例对应的usart_handle
    usart_module_callback module_callback; // 解析收到的数据的回调函数

    /* Optional DMA settings; set channel to USART_DMA_CHANNEL_NONE to disable */
    uint8_t dma_rx_channel;
    uint8_t dma_tx_channel;
    uint32_t rx_frame_gap_us;
} USART_Init_Config_s;

/**
 * @brief 注册一个串口实例,返回一个串口实例指针
 * @note  中断掩码、DMA触发源、NVIC与定时器基础配置建议由SysConfig图形化完成
 *
 * @param init_config 传入串口初始化结构体
 */
USARTInstance *USARTRegister(USART_Init_Config_s *init_config);

/**
 * @brief 启动串口服务,需要传入一个usart实例.一般用于lost callback的情况(使用串口的模块daemon)
 * @note  默认启动DMA接收,并等待外部1ms节拍调用USARTServiceTick1ms进行不定长判帧
 *
 * @param _instance
 */
void USARTServiceInit(USARTInstance *_instance);


/**
 * @brief 通过调用该函数可以发送一帧数据,需要传入一个usart实例,发送buff以及这一帧的长度
 * @note 在短时间内连续调用此接口,若采用IT/DMA会导致上一次的发送未完成而新的发送取消.
 * @note 若希望连续使用DMA/IT进行发送,请配合USARTIsReady()使用,或自行为你的module实现一个发送队列和任务.
 * @todo 是否考虑为USARTInstance增加发送队列以进行连续发送?
 * 
 * @param _instance 串口实例
 * @param send_buf 待发送数据的buffer
 * @param send_size how many bytes to send
 */
void USARTSend(USARTInstance *_instance, uint8_t *send_buf, uint16_t send_size,USART_TRANSFER_MODE mode);

/**
 * @brief 判断串口是否准备好,用于连续或异步的IT/DMA发送
 *
 * @param _instance 要判断的串口实例
 * @return uint8_t ready 1, busy 0
 */
uint8_t USARTIsReady(USARTInstance *_instance);


/**
 * @brief 微秒节拍入口: 允许外部按任意节拍驱动判帧
 * @param elapsed_us 本次调用代表经过的微秒数
 */
void USARTServiceTickUs(uint16_t elapsed_us);

#endif
