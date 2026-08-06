/**
 * @file    taskmgr.h
 * @brief   串口屏任务组状态机框架
 * @author  neozng
 * @date    2026-07-16
 *
 * 通过 UART 接收 TJC 串口屏命令，管理任务组的进入/退出和数据分发。
 * 同一时间只有一个任务组活跃。
 *
 * 命令协议 (ASCII 字符串):
 *   ENTER <id>   - 进入指定任务组
 *   EXIT         - 退出当前任务组
 *   DATA <data>  - 向当前任务组发送数据
 *
 * 使用示例:
 * @code
 *   USARTInstance *usart = USARTRegister(&screen_uart);
 *   TJCUARTScreenInit(usart);
 *
 *   static const TaskGroupDef groups[] = {
 *       { .group_id = 1, .group_name = "homing",
 *         .task_count = 1, .tasks = homing_tasks },
 *   };
 *   static uint8_t fb[128];
 *   TaskMgrInitConfig_s cfg = {
 *       .usart = usart, .groups = groups, .group_count = 1,
 *       .frame_buf = fb, .frame_buf_size = 128,
 *   };
 *   TaskMgrInit(&cfg);
 * @endcode
 */

#ifndef TASK_MGR_H
#define TASK_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp_usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 常量 ========== */

#define TASK_MGR_MAX_DATA_LEN   64      /**< DATA 命令最大负载长度 */
#define TASK_MGR_MAX_CMD_LEN    128     /**< 接收命令最大长度       */
#define TASK_MGR_QUEUE_LENGTH   4       /**< 数据队列深度           */
#define TASK_MGR_PARSER_STACK   (configMINIMAL_STACK_SIZE + 256)
#define TASK_MGR_PARSER_PRIO    2       /**< 解析任务优先级         */

/* ========== 类型定义 ========== */

/** 状态机状态 */
typedef enum {
    TASK_MGR_STATE_IDLE   = 0,  /**< 无活跃任务组 */
    TASK_MGR_STATE_ACTIVE = 1,  /**< 任务组活跃中 */
} TaskMgrState_e;

/** 任务组内单个 FreeRTOS 任务的定义 */
typedef struct {
    TaskFunction_t         task_func;   /**< 任务入口函数 */
    const char            *task_name;   /**< 任务名称     */
    configSTACK_DEPTH_TYPE stack_size;  /**< 栈大小(字)  */
    UBaseType_t            priority;    /**< 优先级       */
} TaskGroupTaskDef;

/** 任务参数 (传给组内每个任务的 pvParameters) */
typedef struct {
    QueueHandle_t  data_queue;  /**< 组数据队列句柄 */
    void          *context;     /**< 用户透传上下文 */
    uint32_t       group_id;    /**< 任务组 ID     */
    uint8_t        task_index;  /**< 在任务组中的索引 */
} TaskGroupTaskParam;

/** 数据消息 (通过队列传递) */
typedef struct {
    char     data[TASK_MGR_MAX_DATA_LEN + 1];
    uint16_t len;
} TaskMgrDataMsg;

/** 任务组进入/退出回调 */
typedef void (*TaskGroupOnEnter)(void *context);
typedef void (*TaskGroupOnExit)(void *context);

/** 任务组定义 (用户提供静态常量数组) */
typedef struct {
    uint32_t             group_id;    /**< 唯一 ID               */
    const char          *group_name;  /**< 可读名称               */
    TaskGroupOnEnter     on_enter;    /**< 进入回调, 可为 NULL   */
    TaskGroupOnExit      on_exit;     /**< 退出回调, 可为 NULL   */
    uint8_t              task_count;  /**< 任务数量               */
    const TaskGroupTaskDef *tasks;    /**< 任务定义数组, 可为 NULL */
    void                *context;     /**< 透传给回调             */
} TaskGroupDef;

/** IDLE 状态下非 ENTER 命令的处理函数 */
typedef void (*TaskMgrMiscCmd)(const char *cmd, uint16_t len);

/** 初始化配置 */
typedef struct {
    USARTInstance        *usart;             /**< 屏幕 UART 实例(已注册) */
    const TaskGroupDef   *groups;            /**< 任务组定义数组         */
    uint8_t               group_count;       /**< 任务组数量             */
    TaskMgrMiscCmd        misc_cmd_handler;  /**< 杂项命令处理, 可为 NULL */
    uint8_t              *frame_buf;         /**< ISR 影子缓冲区         */
    uint16_t              frame_buf_size;    /**< 影子缓冲区大小 (>=128) */
} TaskMgrInitConfig_s;

/* ========== 公开 API ========== */

/**
 * @brief 初始化任务组管理
 *
 * 接管 USART 实例的 module_callback，创建命令解析 FreeRTOS 任务。
 * USART 实例必须已通过 USARTRegister() 注册。
 *
 * @param config  初始化配置 (必须持久存在)
 * @return        0 成功, 负值错误码
 */
int32_t TaskMgrInit(TaskMgrInitConfig_s *config);

#ifdef __cplusplus
}
#endif

#endif /* TASK_MGR_H */
