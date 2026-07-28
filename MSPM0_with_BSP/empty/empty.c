#if 0
#include "ti_msp_dl_config.h"

#include "bsp_log.h"
#include "bsp_time.h"
#include "bsp_usart.h"
#include "third_party/nanoMODBUS/nmbs/port.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* FreeRTOS port 依赖该全局符号 */
uint32_t SystemCoreClock = CPUCLK_FREQ;

#define SLAVE_ID            (1U)        /* 电机 Modbus 地址 */
#define POLL_INTERVAL_MS    (1000U)     /* 发送间隔 */

/* 电机控制参数 */
#define MOTOR_TARGET_SPEED  (100U)      /* 目标速度 100 RPM */
#define MOTOR_ACCEL         (100U)      /* 加速度 100 RPM/s */
#define MOTOR_RESERVED      (0U)        /* 保留 */

/* ========================================================================== *
 *  Modbus 消息结构，通过 FreeRTOS 队列在任务间传递                          *
 * ========================================================================== */
typedef struct {
    uint16_t reg_addr;
    uint16_t reg_values[3];
    uint16_t reg_count;
    uint32_t timestamp_ms;      /* TimeGetBootMs() 时间戳 */
    int      error_code;        /* nmbs_error, 0 = 成功 */
    uint32_t seq;               /* 发送序号 */
} ModbusMsg_t;

static nmbs_t       g_modbus;           /* Modbus 客户端实例 */
static QueueHandle_t xModbusQueue;      /* 消息队列句柄 */

static uint8_t g_uart_rx_buf[NMBS_PORT_FRAME_BUF_SIZE];

static USART_Init_Config_s uart_cfg = {
    .recv_buff      = g_uart_rx_buf,
    .recv_buff_size = NMBS_PORT_FRAME_BUF_SIZE,
    .usart_handle   = UART_0_INST,
    .module_callback = NULL,
    .dma_rx_channel = 0,        /* DMA_CH1_CHAN_ID, UART0_RX */
    .dma_tx_channel = 1,        /* DMA_CH0_CHAN_ID, UART0_TX */
    .rx_frame_gap_us = 5000U,
};

/* ========================================================================== *
 *  发送任务：按固定间隔写 Modbus 多个寄存器，将结果通过队列发往接收任务     *
 * ========================================================================== */
static void vSenderTask(void *pvParameters)
{
    uint32_t last_ms = 0;
    uint32_t seq     = 0;

    (void)pvParameters;

    for (;;)
    {
        uint32_t now = TimeGetBootMs();

        if ((now - last_ms) >= POLL_INTERVAL_MS)
        {
            nmbs_error err;

            last_ms = now;

            /* === FC16 (0x10): 写多个寄存器 ===
             *
             * 01 10 00 F6 00 03 06 00 64 00 64 00 00
             *
             * 起始地址: 0x00F6
             * 寄存器数: 3
             *   [0] 目标速度: 0x0064 = 100 RPM
             *   [1] 加速度:   0x0064 = 100 RPM/s
             *   [2] 保留:     0x0000
             */
            uint16_t regs[3] = {
                MOTOR_TARGET_SPEED, /* 目标速度 */
                MOTOR_ACCEL,        /* 加速度 */
                MOTOR_RESERVED,     /* 保留 */
            };

            seq++;

            err = nmbs_write_multiple_registers(&g_modbus, 0x00F6, 3, regs);

            /* 打包消息发往队列 */
            ModbusMsg_t msg = {
                .reg_addr    = 0x00F6,
                .reg_values[0] = regs[0],
                .reg_values[1] = regs[1],
                .reg_values[2] = regs[2],
                .reg_count   = 3,
                .timestamp_ms = now,
                .error_code  = (int)err,
                .seq         = seq,
            };

            BaseType_t ret = xQueueSend(xModbusQueue, &msg, 0U);
            if (ret != pdPASS)
            {
                LOGWARNING("Sender[#%lu]: queue FULL, msg dropped!", (unsigned long)seq);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));     /* 100 ms 轮询间隔 */
    }
}

/* ========================================================================== *
 *  接收任务：阻塞等待队列消息，收到后用 LOGINFO 打印发送状态                *
 * ========================================================================== */
static void vReceiverTask(void *pvParameters)
{
    ModbusMsg_t msg;

    (void)pvParameters;

    for (;;)
    {
        /* 阻塞等待，直到收到消息 */
        if (xQueueReceive(xModbusQueue, &msg, portMAX_DELAY) == pdPASS)
        {
            if (msg.error_code == NMBS_ERROR_NONE)
            {
                LOGINFO("FC16 OK: [#%lu] speed=%u accel=%u @%lu ms",
                        (unsigned long)msg.seq,
                        msg.reg_values[0],
                        msg.reg_values[1],
                        (unsigned long)msg.timestamp_ms);
            }
            else
            {
                LOGINFO("FC16 failed: [#%lu] err=%d @%lu ms",
                        (unsigned long)msg.seq,
                        msg.error_code,
                        (unsigned long)msg.timestamp_ms);
            }
        }
    }
}

int main(void)
{
    SYSCFG_DL_init();
    NVIC_EnableIRQ(UART_0_INST_INT_IRQN);
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);
    TimeInit();
    BSPLogInit();

    LOGINFO("Modbus motor control demo (FreeRTOS)");

    USARTInstance *usart = USARTRegister(&uart_cfg);

    {
        nmbs_error err = nmbs_client_init(&g_modbus, usart, 1, 1);
        if (err != NMBS_ERROR_NONE)
        {
            LOGERROR("nmbs_client_init failed: %d", (int)err);
            while (1);
        }
    }
    nmbs_set_destination_rtu_address(&g_modbus, SLAVE_ID);

    LOGINFO("Modbus master ready, motor slave=%u", SLAVE_ID);

    /* 创建消息队列（最多存放 5 条消息） */
    xModbusQueue = xQueueCreate(5, sizeof(ModbusMsg_t));
    if (xModbusQueue == NULL)
    {
        LOGERROR("xQueueCreate failed!");
        while (1);
    }

    /* 创建发送任务（优先级 2） */
    if (xTaskCreate(vSenderTask, "send",
                    configMINIMAL_STACK_SIZE + 128, NULL, 2, NULL) != pdPASS)
    {
        LOGERROR("Failed to create sender task!");
        while (1);
    }

    /* 创建接收任务（优先级 1） */
    if (xTaskCreate(vReceiverTask, "recv",
                    configMINIMAL_STACK_SIZE + 128, NULL, 1, NULL) != pdPASS)
    {
        LOGERROR("Failed to create receiver task!");
        while (1);
    }

    LOGINFO("Starting FreeRTOS scheduler...");
    vTaskStartScheduler();

    /* 正常情况下不会执行到这里 */
    LOGERROR("Scheduler exited unexpectedly!");
    while (1);
}

void TIMER_0_INST_IRQHandler(void)
{
    DL_TimerG_clearInterruptStatus(TIMER_0_INST, DL_TIMERG_INTERRUPT_ZERO_EVENT);
    USARTServiceTickUs(1000U);
}
#endif