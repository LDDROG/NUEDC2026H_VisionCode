#include "nmbs/port.h"

#include "bsp_log.h"
#include "bsp_time.h"
#include "nmbs/nmbs_ring_queue.h"

#include <string.h>

/* ========== 前向声明 ========== */

static void   NMBSPortOnFrameReady(void);
static nmbs_error server_write_multiple_coils(uint16_t address, uint16_t quantity,
                                             const nmbs_bitfield coils, uint8_t unit_id, void *arg);
static nmbs_error server_write_multiple_registers(uint16_t address, uint16_t quantity,
                                                  const uint16_t *registers, uint8_t unit_id, void *arg);

/* ========== 内部常量 ========== */

#define NMBS_PORT_MAX_INSTANCES 4U        /* 与 DEVICE_USART_CNT 对齐 */
#define NMBS_PORT_DEFAULT_BYTE_TO 100     /* 默认字节间超时 (ms) */
#define NMBS_PORT_DEFAULT_READ_TO 1000    /* 默认帧读取超时 (ms) */

/* ========== Port 上下文 ========== */

typedef struct
{
    USARTInstance *usart;                          /* 占用的 USART 实例 (NULL=空闲) */
    NMBSRingQueue  rx_queue;
    uint8_t        rx_ring_mem[NMBS_PORT_RX_RING_SIZE];
    uint8_t        rx_frame_mem[NMBS_PORT_FRAME_BUF_SIZE];
    nmbs_server_t *server;                         /* 服务端数据 (客户端为 NULL) */
    uint32_t       dropped_bytes;
    uint8_t        is_client;
    int32_t        byte_timeout_ms;
    int32_t        read_timeout_ms;
} NMBSPortContext;

static NMBSPortContext g_ctx_pool[NMBS_PORT_MAX_INSTANCES];  /* BSS 零初始化 */

/* ========== 上下文管理 ========== */

/** 从池中分配一个空闲上下文 */
static NMBSPortContext *ctx_alloc(void)
{
    uint8_t i;
    for (i = 0U; i < NMBS_PORT_MAX_INSTANCES; i++)
    {
        if (g_ctx_pool[i].usart == NULL)
        {
            return &g_ctx_pool[i];
        }
    }
    return NULL;
}

/** 扫描池中 last_rx_size > 0 的上下文 (ISR 回调分发用) */
static NMBSPortContext *ctx_find_active(void)
{
    uint8_t i;
    for (i = 0U; i < NMBS_PORT_MAX_INSTANCES; i++)
    {
        NMBSPortContext *ctx = &g_ctx_pool[i];
        if ((ctx->usart != NULL) && (ctx->usart->last_rx_size > 0U))
        {
            return ctx;
        }
    }
    return NULL;
}

/** 从回调 arg 中获取匹配的服务端数据 */
static nmbs_server_t *get_server(NMBSPortContext *ctx, uint8_t unit_id)
{
    if ((ctx != NULL) && (ctx->server != NULL) && (ctx->server->id == unit_id))
    {
        return ctx->server;
    }
    return NULL;
}

/* ========== 统一 Port 初始化 (server / client 共用) ========== */

static nmbs_error port_init(NMBSPortContext *ctx, USARTInstance *usart,
                            int32_t byte_timeout_ms, int32_t read_timeout_ms)
{
    if (!NMBSRingQueueInit(&ctx->rx_queue, ctx->rx_ring_mem, NMBS_PORT_RX_RING_SIZE))
    {
        return NMBS_ERROR_INVALID_ARGUMENT;
    }
    NMBSRingQueueReset(&ctx->rx_queue);

    ctx->usart          = usart;
    ctx->byte_timeout_ms = (byte_timeout_ms > 0) ? byte_timeout_ms : NMBS_PORT_DEFAULT_BYTE_TO;
    ctx->read_timeout_ms = (read_timeout_ms > 0) ? read_timeout_ms : NMBS_PORT_DEFAULT_READ_TO;

    /* 接管 USART 接收缓冲区 & 回调 */
    usart->recv_buff      = ctx->rx_frame_mem;
    usart->recv_buff_size = NMBS_PORT_FRAME_BUF_SIZE;
    usart->module_callback = NMBSPortOnFrameReady;

    USARTServiceInit(usart);  /* 使用新缓冲区重启 DMA 接收 */
    return NMBS_ERROR_NONE;
}

/* ========== nanoMODBUS 传输回调 ========== */

/**
 * @brief 平台读回调: 从环形队列中阻塞/超时读取数据
 */
static int32_t read_serial(uint8_t *buf, uint16_t count, int32_t byte_timeout_ms, void *arg)
{
    NMBSPortContext *ctx;
    uint16_t read_size;
    uint32_t wait_start;

    if ((buf == NULL) || (count == 0U))
    {
        return 0;
    }

    ctx = (NMBSPortContext *)arg;
    if ((ctx == NULL) || (ctx->usart == NULL))
    {
        return -1;
    }

    read_size  = 0U;
    wait_start = TimeGetBootMs();

    while (read_size < count)
    {
        uint16_t pulled = NMBSRingQueuePopBytes(&ctx->rx_queue,
                                                &buf[read_size],
                                                (uint16_t)(count - read_size));
        if (pulled > 0U)
        {
            read_size   = (uint16_t)(read_size + pulled);
            wait_start  = TimeGetBootMs();
            continue;
        }

        if (byte_timeout_ms == 0)
        {
            break;
        }

        if ((byte_timeout_ms > 0) &&
            ((TimeGetBootMs() - wait_start) >= (uint32_t)byte_timeout_ms))
        {
            break;
        }
    }

    return (int32_t)read_size;
}

/**
 * @brief 平台写回调: 通过 USART 发送数据 (DMA 优先, 无 DMA 回退阻塞)
 */
static int32_t write_serial(const uint8_t *buf, uint16_t count, int32_t byte_timeout_ms, void *arg)
{
    NMBSPortContext *ctx;

    if ((buf == NULL) || (count == 0U))
    {
        return 0;
    }

    ctx = (NMBSPortContext *)arg;
    if ((ctx == NULL) || (ctx->usart == NULL))
    {
        return -1;
    }

    /* RTU 半双工: 客户端发送前清除接收残留 */
    if (ctx->is_client)
    {
        NMBSRingQueueReset(&ctx->rx_queue);
    }

    /* 等待 USART 发送就绪 */
    if (byte_timeout_ms == 0)
    {
        if (!USARTIsReady(ctx->usart))
        {
            return 0;
        }
    }
    else if (byte_timeout_ms < 0)
    {
        while (!USARTIsReady(ctx->usart)) {}
    }
    else
    {
        uint32_t start = TimeGetBootMs();
        while (!USARTIsReady(ctx->usart))
        {
            if ((TimeGetBootMs() - start) >= (uint32_t)byte_timeout_ms)
            {
                return 0;
            }
        }
    }

    /* DMA 发送; 未配置 DMA TX 通道则回退阻塞发送 */
    if (ctx->usart->dma_tx_channel != USART_DMA_CHANNEL_NONE)
    {
        USARTSend(ctx->usart, (uint8_t *)buf, count, USART_TRANSFER_DMA);
    }
    else
    {
        USARTSend(ctx->usart, (uint8_t *)buf, count, USART_TRANSFER_BLOCKING);
    }

    return (int32_t)count;
}

/* ========== ISR 帧就绪回调 ========== */

/**
 * @brief USART 帧接收完成回调
 *
 * 扫描上下文池中 last_rx_size > 0 的实例 (USARTFinishFrame 在调用本回调前设置),
 * 将帧数据推入对应环形队列. 支持多实例.
 */
static void NMBSPortOnFrameReady(void)
{
    NMBSPortContext *ctx;
    uint16_t size;
    uint16_t pushed;

    ctx = ctx_find_active();
    if ((ctx == NULL) || (ctx->usart == NULL) || (ctx->usart->recv_buff == NULL))
    {
        return;
    }

    size = ctx->usart->last_rx_size;
    ctx->usart->last_rx_size = 0U;  /* 消费, 避免下次回调误匹配 */

    if (size > NMBS_PORT_FRAME_BUF_SIZE)
    {
        size = NMBS_PORT_FRAME_BUF_SIZE;
    }

    if (size == 0U)
    {
        return;
    }

    pushed = NMBSRingQueuePushBytes(&ctx->rx_queue, ctx->usart->recv_buff, size);
    if (pushed < size)
    {
        ctx->dropped_bytes += (uint32_t)(size - pushed);
        LOGWARNING("[nmbs_port] RX queue overflow, dropped=%lu", ctx->dropped_bytes);
    }
}

/* ========== 服务端回调实现 ========== */

static nmbs_error server_read_coils(uint16_t address, uint16_t quantity,
                                    nmbs_bitfield coils_out, uint8_t unit_id, void *arg)
{
    nmbs_server_t *s = get_server((NMBSPortContext *)arg, unit_id);
    uint16_t i;

    if (s == NULL) { return NMBS_ERROR_INVALID_UNIT_ID; }

    for (i = 0U; i < quantity; i++)
    {
        if (address >= (uint16_t)(COIL_BUF_SIZE * 8U))
        {
            return NMBS_ERROR_INVALID_REQUEST;
        }
        nmbs_bitfield_write(coils_out, address, nmbs_bitfield_read(s->coils, address));
        address++;
    }
    return NMBS_ERROR_NONE;
}

static nmbs_error server_read_holding_registers(uint16_t address, uint16_t quantity,
                                                uint16_t *registers_out, uint8_t unit_id, void *arg)
{
    nmbs_server_t *s = get_server((NMBSPortContext *)arg, unit_id);
    uint16_t i;

    if (s == NULL) { return NMBS_ERROR_INVALID_UNIT_ID; }

    for (i = 0U; i < quantity; i++)
    {
        if (address >= REG_BUF_SIZE)
        {
            return NMBS_ERROR_INVALID_REQUEST;
        }
        registers_out[i] = s->regs[address++];
    }
    return NMBS_ERROR_NONE;
}

static nmbs_error server_write_single_coil(uint16_t address, bool value,
                                           uint8_t unit_id, void *arg)
{
    uint8_t coil = value ? 0x01U : 0x00U;
    return server_write_multiple_coils(address, 1U, &coil, unit_id, arg);
}

static nmbs_error server_write_multiple_coils(uint16_t address, uint16_t quantity,
                                              const nmbs_bitfield coils, uint8_t unit_id, void *arg)
{
    nmbs_server_t *s = get_server((NMBSPortContext *)arg, unit_id);
    uint16_t i;

    if (s == NULL) { return NMBS_ERROR_INVALID_UNIT_ID; }

    for (i = 0U; i < quantity; i++)
    {
        if (address >= (uint16_t)(COIL_BUF_SIZE * 8U))
        {
            return NMBS_ERROR_INVALID_REQUEST;
        }
        nmbs_bitfield_write(s->coils, address, nmbs_bitfield_read(coils, i));
        address++;
    }
    return NMBS_ERROR_NONE;
}

static nmbs_error server_write_single_register(uint16_t address, uint16_t value,
                                               uint8_t unit_id, void *arg)
{
    return server_write_multiple_registers(address, 1U, &value, unit_id, arg);
}

static nmbs_error server_write_multiple_registers(uint16_t address, uint16_t quantity,
                                                  const uint16_t *registers, uint8_t unit_id, void *arg)
{
    nmbs_server_t *s = get_server((NMBSPortContext *)arg, unit_id);
    uint16_t i;

    if (s == NULL) { return NMBS_ERROR_INVALID_UNIT_ID; }

    for (i = 0U; i < quantity; i++)
    {
        if (address >= REG_BUF_SIZE)
        {
            return NMBS_ERROR_INVALID_REQUEST;
        }
        s->regs[address++] = registers[i];
    }
    return NMBS_ERROR_NONE;
}

/* ========== 公开 API ========== */

/**
 * @brief 在指定 USART 实例上创建 Modbus RTU 服务端
 */
nmbs_error nmbs_server_init(nmbs_t *nmbs, nmbs_server_t *server, USARTInstance *usart,
                            int32_t byte_timeout_ms, int32_t read_timeout_ms)
{
    nmbs_platform_conf conf;
    nmbs_callbacks     cb;
    nmbs_error         status;
    NMBSPortContext   *ctx;

    if ((nmbs == NULL) || (server == NULL) || (usart == NULL))
    {
        return NMBS_ERROR_INVALID_ARGUMENT;
    }

    if ((server->id == 0U) || (server->id > 247U))
    {
        return NMBS_ERROR_INVALID_ARGUMENT;
    }

    ctx = ctx_alloc();
    if (ctx == NULL)
    {
        return NMBS_ERROR_TRANSPORT;
    }

    status = port_init(ctx, usart, byte_timeout_ms, read_timeout_ms);
    if (status != NMBS_ERROR_NONE)
    {
        return status;
    }

    ctx->server   = server;
    ctx->is_client = 0U;

    /* 构建 nanoMODBUS 平台 & 回调 */
    nmbs_platform_conf_create(&conf);
    conf.transport = NMBS_TRANSPORT_RTU;
    conf.read      = read_serial;
    conf.write     = write_serial;
    conf.arg       = ctx;

    nmbs_callbacks_create(&cb);
    cb.read_coils                = server_read_coils;
    cb.read_holding_registers    = server_read_holding_registers;
    cb.write_single_coil         = server_write_single_coil;
    cb.write_multiple_coils      = server_write_multiple_coils;
    cb.write_single_register     = server_write_single_register;
    cb.write_multiple_registers  = server_write_multiple_registers;
    cb.arg                       = ctx;

    status = nmbs_server_create(nmbs, server->id, &conf, &cb);
    if (status != NMBS_ERROR_NONE)
    {
        ctx->usart = NULL;  /* 释放槽位 */
        return status;
    }

    nmbs_set_byte_timeout(nmbs, ctx->byte_timeout_ms);
    nmbs_set_read_timeout(nmbs, ctx->read_timeout_ms);

    return NMBS_ERROR_NONE;
}

/**
 * @brief 在指定 USART 实例上创建 Modbus RTU 客户端
 */
nmbs_error nmbs_client_init(nmbs_t *nmbs, USARTInstance *usart,
                            int32_t byte_timeout_ms, int32_t read_timeout_ms)
{
    nmbs_platform_conf conf;
    nmbs_error         status;
    NMBSPortContext   *ctx;

    if ((nmbs == NULL) || (usart == NULL))
    {
        return NMBS_ERROR_INVALID_ARGUMENT;
    }

    ctx = ctx_alloc();
    if (ctx == NULL)
    {
        return NMBS_ERROR_TRANSPORT;
    }

    status = port_init(ctx, usart, byte_timeout_ms, read_timeout_ms);
    if (status != NMBS_ERROR_NONE)
    {
        return status;
    }

    ctx->is_client = 1U;

    nmbs_platform_conf_create(&conf);
    conf.transport = NMBS_TRANSPORT_RTU;
    conf.read      = read_serial;
    conf.write     = write_serial;
    conf.arg       = ctx;

    status = nmbs_client_create(nmbs, &conf);
    if (status != NMBS_ERROR_NONE)
    {
        ctx->usart = NULL;
        return status;
    }

    nmbs_set_byte_timeout(nmbs, ctx->byte_timeout_ms);
    nmbs_set_read_timeout(nmbs, ctx->read_timeout_ms);

    return NMBS_ERROR_NONE;
}
