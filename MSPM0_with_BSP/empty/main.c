#include "ti_msp_dl_config.h"

#include "bsp_log.h"
#include "bsp_time.h"
#include "bsp_usart.h"
#include "bsp_pwm.h"

#include <string.h>
#include <stdlib.h>

/* ========================================================================== *
 *  Constants
 * ========================================================================== */
#define PWM_FREQ_HZ       10000.0f
#define PWM_PERIOD_S      (1.0f / PWM_FREQ_HZ)

#define UART_RX_BUF_SIZE  256

/* ========================================================================== *
 *  Globals
 * ========================================================================== */
static PWMInstance   *g_pwm;
static USARTInstance *g_uart;
static float          g_duty = 0.0f;

static uint8_t g_uart_rx_buf[UART_RX_BUF_SIZE];

/* deferred UART response — sent from main loop to stay out of ISR */
static volatile uint8_t g_resp_pending;
static char             g_resp_buf[128];

/* ========================================================================== *
 *  UART command callback (invoked by BSP on each received frame)
 * ========================================================================== */
static void UARTCommandCallback(void)
{
    char    *cmd = (char *)g_uart->recv_buff;
    uint16_t len = g_uart->last_rx_size;

    /* strip trailing whitespace / CR / LF */
    while (len > 0 && (cmd[len - 1] == '\r' || cmd[len - 1] == '\n'))
        --len;
    cmd[len] = '\0';

    if (len == 0) return;

    if (strncmp(cmd, "PWM=", 4) == 0)
    {
        int val = atoi(cmd + 4);
        if (val < 0)   val = 0;
        if (val > 100) val = 100;
        g_duty = (float)val / 100.0f;
        PWMSetDutyRatio(g_pwm, g_duty);

        int slen = sprintf(g_resp_buf, "OK PWM=%d%%\r\n", val);
        g_resp_buf[slen] = '\0';
        g_resp_pending   = 1;
        LOGINFO("PWM set to %d%%", val);
    }
    else if (strcmp(cmd, "PWM?") == 0)
    {
        int pct = (int)(g_duty * 100.0f);
        int slen = sprintf(g_resp_buf, "PWM=%d%%\r\n", pct);
        g_resp_buf[slen] = '\0';
        g_resp_pending   = 1;
        LOGINFO("PWM query: %d%%", pct);
    }
    else if (strcmp(cmd, "ON") == 0)
    {
        g_duty = 1.0f;
        PWMSetDutyRatio(g_pwm, g_duty);
        int slen = sprintf(g_resp_buf, "OK FULL ON\r\n");
        g_resp_buf[slen] = '\0';
        g_resp_pending   = 1;
        LOGINFO("PWM: FULL ON (100%%)");
    }
    else if (strcmp(cmd, "OFF") == 0)
    {
        g_duty = 0.0f;
        PWMSetDutyRatio(g_pwm, g_duty);
        int slen = sprintf(g_resp_buf, "OK OFF\r\n");
        g_resp_buf[slen] = '\0';
        g_resp_pending   = 1;
        LOGINFO("PWM: OFF (0%%)");
    }
    else
    {
        int slen = sprintf(g_resp_buf, "ERR unknown cmd: %s\r\n", cmd);
        g_resp_buf[slen] = '\0';
        g_resp_pending   = 1;
        LOGINFO("Unknown cmd: '%s'", cmd);
    }
}

/* ========================================================================== *
 *  main
 * ========================================================================== */
int main(void)
{
    SYSCFG_DL_init();
    NVIC_EnableIRQ(UART_0_INST_INT_IRQN);
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);
    TimeInit();
    BSPLogInit();

    LOGINFO("=== Electromagnet PWM + Vision UART Test ===");
    LOGINFO("MCU: MSPM0G3507 @ 80MHz | PWM: PA0 10kHz | UART: PA10/PA11 1Mbps");

    /* ---- PWM: TIMA0 channel-0 on PA0, 10 kHz ---- */
    PWM_Init_Config_s pwm_cfg = {
        .htim      = PWM_0_INST,
        .channel   = GPIO_PWM_0_C0_IDX,
        .period    = PWM_PERIOD_S,
        .dutyratio = 0.0f,
        .callback  = NULL,
        .id        = NULL,
    };

    g_pwm = PWMRegister(&pwm_cfg);
    if (g_pwm == NULL)
    {
        LOGERROR("PWMRegister failed!");
        while (1);
    }
    LOGINFO("PWM registered on PA0, 10kHz, initially OFF");

    /* ---- UART0: DMA RX, blocking TX ---- */
    USART_Init_Config_s uart_cfg = {
        .recv_buff       = g_uart_rx_buf,
        .recv_buff_size  = UART_RX_BUF_SIZE,
        .usart_handle    = UART_0_INST,
        .module_callback = UARTCommandCallback,
        .dma_rx_channel  = DMA_CH1_CHAN_ID,
        .dma_tx_channel  = USART_DMA_CHANNEL_NONE,
        .rx_frame_gap_us = 5000U,
    };

    g_uart = USARTRegister(&uart_cfg);
    if (g_uart == NULL)
    {
        LOGERROR("USARTRegister failed!");
        while (1);
    }
    LOGINFO("UART0 ready: DMA RX on PA11, TX on PA10, 1Mbps");

    /* startup banner over UART */
    const char *banner =
        "\r\n"
        "============================================\r\n"
        "  MSPM0G3507 Electromagnet + Vision Test\r\n"
        "  PWM : PA0, 10 kHz, 0-100%%\r\n"
        "  UART: PA10(TX) PA11(RX), 1 Mbps\r\n"
        "  Cmds: PWM=<0-100> | PWM? | ON | OFF\r\n"
        "============================================\r\n";
    USARTSend(g_uart, (uint8_t *)banner, strlen(banner), USART_TRANSFER_BLOCKING);

    LOGINFO("Init done. Ready for commands.");

    /* ---- main loop ---- */
    while (1)
    {
        if (g_resp_pending)
        {
            g_resp_pending = 0;
            USARTSend(g_uart, (uint8_t *)g_resp_buf, strlen(g_resp_buf),
                      USART_TRANSFER_BLOCKING);
        }
        __WFI();
    }
}

/* ========================================================================== *
 *  ISR: 1 ms timer tick — drives USART frame detection
 * ========================================================================== */
void TIMER_0_INST_IRQHandler(void)
{
    DL_TimerG_clearInterruptStatus(TIMER_0_INST, DL_TIMERG_INTERRUPT_ZERO_EVENT);
    USARTServiceTickUs(1000U);
}

/* UART0_IRQHandler() is provided by bsp_usart.c — no need to redefine */
