#include "ti_msp_dl_config.h"
#include "bsp_usart.h"

uint32_t SystemCoreClock = CPUCLK_FREQ;

/* ========================================================================== *
 *  极简串口打印（UART0 → XDS110 虚拟串口 → 电脑 COM 口）
 * ========================================================================== */
static void uart0_putc(char c)
{
    DL_UART_transmitDataBlocking(UART_0_INST, c);
}

static void uart0_puts(const char *s)
{
    while (*s) uart0_putc(*s++);
}

static void print_button(uint8_t num)
{
    uart0_puts("Button: ");
    uart0_putc('0' + num);   // num 是 1~8，直接拼成 ASCII
    uart0_puts("\r\n");
}

/* ========================================================================== *
 *  屏幕协议：2 字节二进制帧, [0xAA, 按键号]
 *    按键 1 → [AA 01]   按键 5 → [AA 05]
 *    按键 2 → [AA 02]   按键 6 → [AA 06]
 *    按键 3 → [AA 03]   按键 7 → [AA 07]
 *    按键 4 → [AA 04]   按键 8 → [AA 08]
 * ========================================================================== */

static USARTInstance *g_uart;
static volatile uint8_t g_button;
static volatile uint8_t g_has_new;

/* 收到一帧 → BSP 自动调用 */
static void on_screen_frame(void)
{
    if (g_uart->last_rx_size >= 2
        && g_uart->recv_buff[0] == 0xAA
        && g_uart->recv_buff[1] >= 1
        && g_uart->recv_buff[1] <= 8)
    {
        g_button  = g_uart->recv_buff[1];
        g_has_new = 1;
    }
}

/* ---- 屏幕 UART：硬件 UART2, PB15(TX) PB16(RX), 115200 ---- */
static uint8_t rx_buf[32];

static USART_Init_Config_s screen_uart_cfg = {
    .recv_buff       = rx_buf,
    .recv_buff_size  = sizeof(rx_buf),
    .usart_handle    = UART_1_INST,       // SysConfig 中名为 "UART_1" 的实例
    .module_callback = on_screen_frame,
    .dma_rx_channel  = 5,                 // DMA_CH5
    .dma_tx_channel  = 6,                 // DMA_CH6
    .rx_frame_gap_us = 5000U,
};

/* ========================================================================== *
 *  main
 * ========================================================================== */
int main(void)
{
    SYSCFG_DL_init();

    .usart_handle    = UART_1_INST,   
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);

    uart0_puts("--- Screen Button Test ---\r\n");

    g_uart = USARTRegister(&screen_uart_cfg);

    while (1)
    {
        if (g_has_new)
        {
            g_has_new = 0;
            print_button(g_button);
        }
    }
}

/* 1ms 定时中断 → 驱动 BSP 判帧 */
void TIMER_0_INST_IRQHandler(void)
{
    DL_TimerG_clearInterruptStatus(TIMER_0_INST, DL_TIMERG_INTERRUPT_ZERO_EVENT);
    USARTServiceTickUs(1000U);
}
