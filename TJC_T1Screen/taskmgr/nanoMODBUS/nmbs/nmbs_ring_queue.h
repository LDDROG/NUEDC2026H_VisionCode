#ifndef NMBS_RING_QUEUE_H
#define NMBS_RING_QUEUE_H

#include <stdint.h>

typedef struct
{
    uint8_t *buffer;
    uint16_t capacity;
    uint16_t mask;
    volatile uint16_t head;
    volatile uint16_t tail;
} NMBSRingQueue;

/**
 * @brief 初始化环形队列
 * @param queue 队列对象
 * @param buffer 外部提供的存储区
 * @param capacity 存储区容量,必须是2的幂
 * @return uint8_t 成功返回1,失败返回0
 */
uint8_t NMBSRingQueueInit(NMBSRingQueue *queue, uint8_t *buffer, uint16_t capacity);

/**
 * @brief 复位环形队列
 * @param queue 队列对象
 */
void NMBSRingQueueReset(NMBSRingQueue *queue);

/**
 * @brief 获取当前可读字节数
 * @param queue 队列对象
 * @return uint16_t 可读字节数
 */
uint16_t NMBSRingQueueAvailable(const NMBSRingQueue *queue);

/**
 * @brief 获取当前空闲字节数
 * @param queue 队列对象
 * @return uint16_t 空闲字节数
 */
uint16_t NMBSRingQueueFree(const NMBSRingQueue *queue);

/**
 * @brief 写入一个字节
 * @param queue 队列对象
 * @param byte 待写入字节
 * @return uint8_t 成功返回1,队列满返回0
 */
uint8_t NMBSRingQueuePush(NMBSRingQueue *queue, uint8_t byte);

/**
 * @brief 读出一个字节
 * @param queue 队列对象
 * @param out_byte 读出目标地址
 * @return uint8_t 成功返回1,队列空返回0
 */
uint8_t NMBSRingQueuePop(NMBSRingQueue *queue, uint8_t *out_byte);

/**
 * @brief 批量写入字节
 * @param queue 队列对象
 * @param data 数据源
 * @param size 数据长度
 * @return uint16_t 实际写入字节数
 */
uint16_t NMBSRingQueuePushBytes(NMBSRingQueue *queue, const uint8_t *data, uint16_t size);

/**
 * @brief 批量读出字节
 * @param queue 队列对象
 * @param out_data 数据输出地址
 * @param size 期望读出长度
 * @return uint16_t 实际读出字节数
 */
uint16_t NMBSRingQueuePopBytes(NMBSRingQueue *queue, uint8_t *out_data, uint16_t size);

#endif
