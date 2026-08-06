#include "ti_msp_dl_config.h"

#include "bsp_log.h"
#include "bsp_time.h"
#include "bsp_usart.h"
#include "nanoMODBUS/nmbs/port.h"
#include "wit_port.h"
#include "tjcuartscreen.h"
#include "taskmgr.h"
/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
/* FreeRTOS port 依赖该全局符号 */
uint32_t SystemCoreClock = CPUCLK_FREQ;


/* ========================================================================== *
 *  串口屏任务组测试                                                         *
 * ========================================================================== */

/** 任务组 1 的任务 — "归零模式" */
static void vHomingTask(void *pvParameters)
{
    TaskGroupTaskParam *param = (TaskGroupTaskParam *)pvParameters;
    TaskMgrDataMsg msg;

    LOGINFO("[homing] Task started, waiting for data...");

    for (;;)
    {
        if (xQueueReceive(param->data_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            LOGINFO("[homing] Received: %s (len=%u)", msg.data, (unsigned int)msg.len);
        }
    }
}

/** 任务组 2 的任务 — "跟踪模式" */
static void vTrackingTask(void *pvParameters)
{
    TaskGroupTaskParam *param = (TaskGroupTaskParam *)pvParameters;
    TaskMgrDataMsg msg;

    LOGINFO("[tracking] Task started, waiting for data...");

    for (;;)
    {
        if (xQueueReceive(param->data_queue, &msg, portMAX_DELAY) == pdTRUE)
        {
            LOGINFO("[tracking] Received: %s (len=%u)", msg.data, (unsigned int)msg.len);
        }
    }
}

/** 任务组 1 进入回调 */
static void OnEnterGroup1(void *context)
{
    (void)context;
    LOGINFO("[taskmgr] Group 1 enter callback!");
    HIM_change_string("t0", "Homing");
}

/** 任务组 1 退出回调 */
static void OnExitGroup1(void *context)
{
    (void)context;
    LOGINFO("[taskmgr] Group 1 exit callback!");
    HIM_change_string("t0", "Idle");
}

/** 任务组 2 进入回调 */
static void OnEnterGroup2(void *context)
{
    (void)context;
    LOGINFO("[taskmgr] Group 2 enter callback!");
    HIM_change_string("t0", "Tracking");
}

/** 任务组 2 退出回调 */
static void OnExitGroup2(void *context)
{
    (void)context;
    LOGINFO("[taskmgr] Group 2 exit callback!");
    HIM_change_string("t0", "Idle");
}

/** 组内任务定义 */
static const TaskGroupTaskDef g_homing_tasks[] = {
    { .task_func = vHomingTask, .task_name = "task1",
      .stack_size = 256, .priority = 1 },
};
static const TaskGroupTaskDef g_tracking_tasks[] = {
    { .task_func = vTrackingTask, .task_name = "task2",
      .stack_size = 256, .priority = 1 },
};

/** 任务组定义数组 */
static const TaskGroupDef g_groups[] = {
    { .group_id = 1, .group_name = "homing",
      .on_enter = OnEnterGroup1, .on_exit = OnExitGroup1,
      .task_count = 1, .tasks = g_homing_tasks },
    { .group_id = 2, .group_name = "tracking",
      .on_enter = OnEnterGroup2, .on_exit = OnExitGroup2,
      .task_count = 1, .tasks = g_tracking_tasks },
};

/** IDLE 状态下非 ENTER 命令的处理 */
static void OnMiscCmd(const char *cmd, uint16_t len)
{
    LOGINFO("[taskmgr] Misc cmd: %s", cmd);
}

/**
 * @brief 串口屏 UART 配置
 */
static uint8_t screen_rx_buf[128];
static uint8_t taskmgr_frame_buf[128];
static USART_Init_Config_s screen_uart = {
    .recv_buff      = screen_rx_buf,
    .recv_buff_size = sizeof(screen_rx_buf),
    .usart_handle   = UART_0_INST,
    .module_callback = NULL,
    .dma_rx_channel = 0,        /* DMA_CH1_CHAN_ID, UART0_RX */
    .dma_tx_channel = 1,        /* DMA_CH0_CHAN_ID, UART0_TX */
    .rx_frame_gap_us = 5000U,
};

int main(void)
{
    SYSCFG_DL_init();
    NVIC_EnableIRQ(UART_0_INST_INT_IRQN);
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);
    TimeInit();
    BSPLogInit();

    LOGINFO("MSPM0G3507 Task Manager Demo");

    USARTInstance *screen = USARTRegister(&screen_uart);
    TJCUARTScreenInit(screen);

    TaskMgrInitConfig_s tm_cfg = {
        .usart           = screen,
        .groups          = g_groups,
        .group_count     = 2U,
        .misc_cmd_handler = OnMiscCmd,
        .frame_buf       = taskmgr_frame_buf,
        .frame_buf_size  = sizeof(taskmgr_frame_buf),
    };
    TaskMgrInit(&tm_cfg);


    LOGINFO("Starting FreeRTOS scheduler...");
    vTaskStartScheduler();

    LOGERROR("Scheduler exited unexpectedly!");
    while (1);
}

void TIMER_0_INST_IRQHandler(void)
{
    DL_TimerG_clearInterruptStatus(TIMER_0_INST, DL_TIMERG_INTERRUPT_ZERO_EVENT);
    USARTServiceTickUs(1000U);
}
