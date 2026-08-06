/**
 * @file    taskmgr.c
 * @brief   串口屏任务组状态机框架实现
 * @author  neozng
 * @date    2026-07-16
 *
 * == 数据流 ==
 *
 * RX:
 *   TJC 屏 --UART--> BSP USART(DMA) --frame_detect--> module_callback (ISR)
 *       |                                                    |
 *       |  memcpy(frame_buf)  xSemaphoreGiveFromISR()        |
 *       v                                                    |
 *   TaskMgrParserTask (FreeRTOS) <----------------------------+
 *       |
 *       +--[IDLE]-- ENTER -> TaskMgrEnterGroup -> 创建队列+任务
 *       +--[IDLE]-- 其他   -> misc_cmd_handler()
 *       +--[ACTIVE]- EXIT  -> TaskMgrExitGroup  -> 删除队列+任务
 *       +--[ACTIVE]- DATA  -> xQueueSend(active_queue)
 *       +--[ACTIVE]- ENTER -> Exit + Enter
 *
 * TX (组任务直接调用 TJC 接口):
 *   vGroupTask() -> HIM_change_string() -> USARTSend()
 */

#include "taskmgr.h"
#include "bsp_log.h"
#include "semphr.h"
#include <string.h>
#include <stdlib.h>

/* ========== 内部上下文 ========== */

typedef struct {
    USARTInstance        *usart;
    const TaskGroupDef   *groups;
    uint8_t               group_count;
    TaskMgrMiscCmd        misc_cmd_handler;
    uint8_t              *frame_buf;
    uint16_t              frame_buf_size;
    volatile uint16_t     frame_len;
    volatile bool         frame_ready;
    SemaphoreHandle_t     frame_semaphore;
    TaskMgrState_e        state;

    /* 活跃任务组相关 */
    const TaskGroupDef   *active_group;
    QueueHandle_t         active_queue;
    TaskHandle_t         *active_task_handles;
    void                **active_task_params;   /* 用于退出时释放 */
} TaskMgrCtx;

static TaskMgrCtx g_ctx = {0};

/* ========== 前向声明 ========== */

static void TaskMgrOnFrameReady(void);
static void TaskMgrParserTask(void *pvParameters);
static void TaskMgrProcessCommand(char *cmd, uint16_t len);
static void TaskMgrHandleIdleState(const char *cmd, const char *args);
static void TaskMgrHandleActiveState(const char *cmd, const char *args);
static int32_t TaskMgrEnterGroup(uint32_t group_id);
static void TaskMgrExitGroup(void);
static void TaskMgrDeliverData(const char *data);

/* ========== ISR 回调 ========== */

static void TaskMgrOnFrameReady(void)
{
    USARTInstance *usart = g_ctx.usart;
    uint16_t size;

    if (usart == NULL) return;

    size = usart->last_rx_size;
    usart->last_rx_size = 0U;

    /* 无数据或上一帧尚未消费 */
    if ((size == 0U) || g_ctx.frame_ready)
    {
        return;
    }

    if (size > g_ctx.frame_buf_size)
    {
        size = g_ctx.frame_buf_size;
    }

    memcpy(g_ctx.frame_buf, usart->recv_buff, size);
    g_ctx.frame_len    = size;
    g_ctx.frame_ready  = true;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(g_ctx.frame_semaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* ========== 命令解析 ========== */

/**
 * @brief 去除字符串末尾的控制字符
 */
static void trim_trailing(char *s, uint16_t *len)
{
    while (*len > 0)
    {
        uint8_t c = (uint8_t)s[*len - 1];
        if (c > ' ') break;  /* 非控制字符停止 */
        s[--(*len)] = '\0';
    }
}

static void TaskMgrProcessCommand(char *cmd, uint16_t len)
{
    char *args;
    char *space;

    if (len == 0U) return;

    /* 确保 null-terminated */
    if (len >= TASK_MGR_MAX_CMD_LEN)
    {
        len = TASK_MGR_MAX_CMD_LEN - 1;
    }
    cmd[len] = '\0';

    /* 去尾部控制字符 */
    trim_trailing(cmd, &len);
    if (len == 0U) return;

    /* 第一个空格切分命令与参数 */
    space = strchr(cmd, ' ');
    args  = NULL;
    if (space != NULL)
    {
        *space = '\0';
        args = space + 1;
        while (*args == ' ') args++;  /* 跳过参数前空格 */
        if (*args == '\0') args = NULL;
    }

    if (g_ctx.state == TASK_MGR_STATE_IDLE)
    {
        TaskMgrHandleIdleState(cmd, args);
    }
    else
    {
        TaskMgrHandleActiveState(cmd, args);
    }
}

/* ========== IDLE 状态处理 ========== */

static void TaskMgrHandleIdleState(const char *cmd, const char *args)
{
    if ((strcmp(cmd, "ENTER") == 0) && (args != NULL))
    {
        uint32_t id = (uint32_t)atoi(args);
        TaskMgrEnterGroup(id);
    }
    else
    {
        if (g_ctx.misc_cmd_handler != NULL)
        {
            /* 重建完整命令 */
            char full[TASK_MGR_MAX_CMD_LEN];
            uint16_t n;
            if (args != NULL)
            {
                n = (uint16_t)snprintf(full, sizeof(full), "%s %s", cmd, args);
            }
            else
            {
                n = (uint16_t)snprintf(full, sizeof(full), "%s", cmd);
            }
            g_ctx.misc_cmd_handler(full, n);
        }
    }
}

/* ========== ACTIVE 状态处理 ========== */

static void TaskMgrHandleActiveState(const char *cmd, const char *args)
{
    if (strcmp(cmd, "EXIT") == 0)
    {
        TaskMgrExitGroup();
    }
    else if ((strcmp(cmd, "DATA") == 0) && (args != NULL))
    {
        TaskMgrDeliverData(args);
    }
    else if ((strcmp(cmd, "ENTER") == 0) && (args != NULL))
    {
        /* 退出当前组，进入新组 */
        uint32_t id = (uint32_t)atoi(args);
        TaskMgrExitGroup();
        TaskMgrEnterGroup(id);
    }
    else
    {
        /* ACTIVE 状态下不识别的命令忽略 */
        LOGWARNING("[taskmgr] Unknown command in ACTIVE: %s", cmd);
    }
}

/* ========== 任务组生命周期 ========== */

static int32_t TaskMgrEnterGroup(uint32_t group_id)
{
    const TaskGroupDef *group = NULL;
    uint8_t i;

    if (g_ctx.state == TASK_MGR_STATE_ACTIVE)
    {
        LOGWARNING("[taskmgr] Already in group, cannot ENTER");
        return -1;
    }

    /* 查找 group 定义 */
    for (i = 0U; i < g_ctx.group_count; i++)
    {
        if (g_ctx.groups[i].group_id == group_id)
        {
            group = &g_ctx.groups[i];
            break;
        }
    }
    if (group == NULL)
    {
        LOGWARNING("[taskmgr] Unknown group ID: %lu", (unsigned long)group_id);
        return -1;
    }

    /* 创建数据队列 */
    QueueHandle_t queue = xQueueCreate(TASK_MGR_QUEUE_LENGTH, sizeof(TaskMgrDataMsg));
    if (queue == NULL)
    {
        LOGERROR("[taskmgr] Queue create failed for group %lu", (unsigned long)group_id);
        return -1;
    }

    /* 分配任务句柄数组 */
    TaskHandle_t *handles = NULL;
    void **params = NULL;
    if (group->task_count > 0U)
    {
        handles = (TaskHandle_t *)pvPortMalloc(group->task_count * sizeof(TaskHandle_t));
        params  = (void **)pvPortMalloc(group->task_count * sizeof(void *));
        if ((handles == NULL) || (params == NULL))
        {
            LOGERROR("[taskmgr] Malloc failed for group %lu", (unsigned long)group_id);
            if (handles) vPortFree(handles);
            if (params)  vPortFree(params);
            vQueueDelete(queue);
            return -1;
        }
        memset(handles, 0, group->task_count * sizeof(TaskHandle_t));
        memset(params,  0, group->task_count * sizeof(void *));
    }

    /* 创建组内任务 */
    for (i = 0U; i < group->task_count; i++)
    {
        TaskGroupTaskParam *param = (TaskGroupTaskParam *)pvPortMalloc(sizeof(TaskGroupTaskParam));
        if (param == NULL) goto cleanup;

        param->data_queue = queue;
        param->context    = group->context;
        param->group_id   = group_id;
        param->task_index = i;
        params[i] = param;

        if (xTaskCreate(group->tasks[i].task_func,
                        group->tasks[i].task_name,
                        group->tasks[i].stack_size,
                        param,
                        group->tasks[i].priority,
                        &handles[i]) != pdPASS)
        {
            vPortFree(param);
            params[i] = NULL;
            goto cleanup;
        }
    }

    /* 保存活跃状态 */
    g_ctx.active_group        = group;
    g_ctx.active_queue        = queue;
    g_ctx.active_task_handles = handles;
    g_ctx.active_task_params  = params;
    g_ctx.state               = TASK_MGR_STATE_ACTIVE;

    /* 回调 on_enter */
    if (group->on_enter != NULL)
    {
        group->on_enter(group->context);
    }

    LOGINFO("[taskmgr] Entered group %lu (%s)",
            (unsigned long)group_id, group->group_name);
    return 0;

cleanup:
    LOGERROR("[taskmgr] Task create failed at index %u", (unsigned int)i);
    for (uint8_t j = 0U; j < i; j++)
    {
        vTaskDelete(handles[j]);
        vPortFree(params[j]);
    }
    if (handles) vPortFree(handles);
    if (params)  vPortFree(params);
    vQueueDelete(queue);
    return -1;
}

static void TaskMgrExitGroup(void)
{
    const TaskGroupDef *group = g_ctx.active_group;

    if (group == NULL) return;

    LOGINFO("[taskmgr] Exiting group %lu (%s)",
            (unsigned long)group->group_id, group->group_name);

    /* 回调 on_exit */
    if (group->on_exit != NULL)
    {
        group->on_exit(group->context);
    }

    /* 删除组内任务, 释放参数 */
    for (uint8_t i = 0U; i < group->task_count; i++)
    {
        if (g_ctx.active_task_handles != NULL)
        {
            vTaskDelete(g_ctx.active_task_handles[i]);
        }
        if ((g_ctx.active_task_params != NULL) && (g_ctx.active_task_params[i] != NULL))
        {
            vPortFree(g_ctx.active_task_params[i]);
        }
    }

    /* 释放数组 */
    if (g_ctx.active_task_handles) vPortFree(g_ctx.active_task_handles);
    if (g_ctx.active_task_params)  vPortFree(g_ctx.active_task_params);

    /* 删除队列 */
    if (g_ctx.active_queue) vQueueDelete(g_ctx.active_queue);

    /* 复位 */
    g_ctx.active_group        = NULL;
    g_ctx.active_queue        = NULL;
    g_ctx.active_task_handles = NULL;
    g_ctx.active_task_params  = NULL;
    g_ctx.state               = TASK_MGR_STATE_IDLE;
}

static void TaskMgrDeliverData(const char *data)
{
    QueueHandle_t queue = g_ctx.active_queue;
    if (queue == NULL) return;

    TaskMgrDataMsg msg;
    uint16_t dlen = (uint16_t)strlen(data);

    if (dlen > TASK_MGR_MAX_DATA_LEN)
    {
        dlen = TASK_MGR_MAX_DATA_LEN;
    }
    memcpy(msg.data, data, dlen);
    msg.data[dlen] = '\0';
    msg.len = dlen;

    if (xQueueSend(queue, &msg, 0U) != pdTRUE)
    {
        LOGWARNING("[taskmgr] Data queue full, dropping: %s", data);
    }
}

/* ========== 解析任务 ========== */

static void TaskMgrParserTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t cmd_buf[TASK_MGR_MAX_CMD_LEN];
    uint16_t cmd_len;

    for (;;)
    {
        /* 等待帧到达信号 */
        xSemaphoreTake(g_ctx.frame_semaphore, portMAX_DELAY);

        /* 临界区拷贝帧数据 */
        taskENTER_CRITICAL();
        cmd_len = g_ctx.frame_len;
        if (cmd_len > sizeof(cmd_buf))
        {
            cmd_len = sizeof(cmd_buf);
        }
        memcpy(cmd_buf, g_ctx.frame_buf, cmd_len);
        g_ctx.frame_ready = false;
        taskEXIT_CRITICAL();

        /* 解析并执行 */
        TaskMgrProcessCommand((char *)cmd_buf, cmd_len);
    }
}

/* ========== 公开 API ========== */

int32_t TaskMgrInit(TaskMgrInitConfig_s *config)
{
    if ((config == NULL) || (config->usart == NULL) ||
        (config->groups == NULL) || (config->frame_buf == NULL))
    {
        return -1;
    }

    if (config->frame_buf_size < 64U)
    {
        return -1;
    }

    /* 不允许重复初始化 */
    if (g_ctx.frame_semaphore != NULL)
    {
        return -1;
    }

    /* 填充上下文 */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.usart            = config->usart;
    g_ctx.groups           = config->groups;
    g_ctx.group_count      = config->group_count;
    g_ctx.misc_cmd_handler = config->misc_cmd_handler;
    g_ctx.frame_buf        = config->frame_buf;
    g_ctx.frame_buf_size   = config->frame_buf_size;
    g_ctx.state            = TASK_MGR_STATE_IDLE;

    /* 创建二值信号量 */
    g_ctx.frame_semaphore = xSemaphoreCreateBinary();
    if (g_ctx.frame_semaphore == NULL)
    {
        LOGERROR("[taskmgr] Semaphore create failed");
        return -1;
    }

    /* 接管 USART 帧回调 */
    config->usart->module_callback = TaskMgrOnFrameReady;
    USARTServiceInit(config->usart);

    /* 创建解析任务 */
    if (xTaskCreate(TaskMgrParserTask, "tMgr", TASK_MGR_PARSER_STACK,
                    NULL, TASK_MGR_PARSER_PRIO, NULL) != pdPASS)
    {
        LOGERROR("[taskmgr] Task create failed");
        vSemaphoreDelete(g_ctx.frame_semaphore);
        g_ctx.frame_semaphore = NULL;
        return -1;
    }

    LOGINFO("[taskmgr] Initialized, %u groups registered",
            (unsigned int)config->group_count);
    return 0;
}
