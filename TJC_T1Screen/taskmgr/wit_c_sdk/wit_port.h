/**
 * @file    wit_port.h
 * @brief   WIT 传感器 BSP 移植层 — 通过项目 BSP USART 对接 WIT SDK
 * @author  neozng
 * @date    2025-XX-XX
 *
 * 本文件提供 WIT SDK (wit_c_sdk) 与硬件 BSP 之间的桥接接口。
 * 用户只需提供已注册的 USARTInstance 指针和协议配置，
 * port 层会自动完成 WIT SDK 的 SerialWrite/DelayMs/Callback 注册，
 * 并接管 BSP USART 的 module_callback 实现数据接收。
 *
 * 使用示例:
 * @code
 *   USARTInstance *usart = USARTRegister(&uart_cfg);
 *   WitPortInitConfig_s cfg = {
 *       .usart          = usart,
 *       .protocol       = WIT_PROTOCOL_NORMAL,
 *       .addr           = 0x50,
 *       .data_callback  = MyDataCallback,
 *   };
 *   WitPortInit(&cfg);
 * @endcode
 *
 * 依赖: bsp/usart (USARTInstance), wit_c_sdk (WIT 协议)
 */

#ifndef WIT_PORT_H
#define WIT_PORT_H

#include <stdint.h>
#include "bsp_usart.h"
#include "wit_c_sdk.h"

/**
 * @brief WIT Port 初始化配置
 */
typedef struct
{
    USARTInstance *usart;           /**< 已通过 USARTRegister() 注册的 USART 实例   */
    uint32_t       protocol;        /**< 协议: WIT_PROTOCOL_NORMAL/MODBUS/JY61/... */
    uint8_t        addr;            /**< 传感器设备地址 (通常 0x50)               */
    RegUpdateCb    data_callback;   /**< 传感器数据更新回调 (注册到 WIT SDK)      */
} WitPortInitConfig_s;

/**
 * @brief 初始化 WIT 传感器移植层
 *
 * 接管指定 USART 实例的 module_callback (注册帧回调)，
 * 注册 WIT SDK 所需的 SerialWrite/DelayMs/Callback 回调，
 * 调用 WitInit(protocol, addr) 设置协议与地址。
 *
 * 注意: Port 层不额外分配 RX 缓冲区，直接复用 USARTRegister()
 *       时用户传入的 recv_buff。请确保该 buffer 大小 >= 256 字节。
 *
 * @param config  初始化配置 (不能为 NULL)
 * @return        WIT_HAL_OK (0)  成功
 *                WIT_HAL_INVAL  参数无效
 *                WIT_HAL_EMPTY  USART 实例忙或 SDK 回调注册失败
 */
int32_t WitPortInit(WitPortInitConfig_s *config);

/**
 * @brief 反初始化 WIT 移植层
 *
 * 调用 WitDeInit() 清空 SDK 内部状态，释放 USART 占用标记。
 */
void WitPortDeInit(void);

#endif /* WIT_PORT_H */
