#ifndef NANOMODBUS_PORT_H
#define NANOMODBUS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "bsp_usart.h"
#include "nanomodbus.h"

/* 线圈/寄存器缓冲区容量 */
#define COIL_BUF_SIZE 64U
#define REG_BUF_SIZE 64U

/* RTU 传输帧缓冲与接收环形队列 */
#define NMBS_PORT_FRAME_BUF_SIZE 256U
#define NMBS_PORT_RX_RING_SIZE   512U

/**
 * @brief Modbus RTU 服务端数据模型
 */
typedef struct
{
    uint8_t id;                     /**< RTU 从机地址 (1-247) */
    uint8_t coils[COIL_BUF_SIZE];   /**< 线圈位域 (8192 coils) */
    uint16_t regs[REG_BUF_SIZE];    /**< 保持寄存器 (2048 regs) */
} nmbs_server_t;

/**
 * @brief 在指定 USART 实例上创建 Modbus RTU 服务端
 *
 * port 层会接管该 USART 的 recv_buff / module_callback / DMA 配置,
 * 用户无需再单独管理帧缓冲区.
 *
 * @param nmbs             nanoMODBUS 实例指针
 * @param server           服务端数据模型 (server->id 须为 1-247)
 * @param usart            已通过 USARTRegister() 注册的 USART 实例
 * @param byte_timeout_ms  字节间超时 (ms), 0 使用默认值 100
 * @param read_timeout_ms  帧读取超时 (ms), 0 使用默认值 1000
 * @return nmbs_error      NMBS_ERROR_NONE 成功, 其他失败
 */
nmbs_error nmbs_server_init(nmbs_t *nmbs, nmbs_server_t *server, USARTInstance *usart,
                            int32_t byte_timeout_ms, int32_t read_timeout_ms);

/**
 * @brief 在指定 USART 实例上创建 Modbus RTU 客户端
 *
 * @param nmbs             nanoMODBUS 实例指针
 * @param usart            已通过 USARTRegister() 注册的 USART 实例
 * @param byte_timeout_ms  字节间超时 (ms), 0 使用默认值 100
 * @param read_timeout_ms  帧读取超时 (ms), 0 使用默认值 1000
 * @return nmbs_error      NMBS_ERROR_NONE 成功, 其他失败
 */
nmbs_error nmbs_client_init(nmbs_t *nmbs, USARTInstance *usart,
                            int32_t byte_timeout_ms, int32_t read_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
