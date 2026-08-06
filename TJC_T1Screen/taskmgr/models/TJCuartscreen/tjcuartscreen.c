#include "tjcuartscreen.h"
#include "printf.h"

#define UART_PRINTF_BUFFER_SIZE 128
USARTInstance *g_usart_screen = NULL;

int printf_DMA(const char *fmt, ...) {
    static char buffer[UART_PRINTF_BUFFER_SIZE];
    va_list args;
    int len;
    
    // 安全检查
    if (fmt == NULL) {
        return 0;
    }
    
    while (!USARTIsReady(g_usart_screen));
    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // 处理缓冲区溢出的情况
    if (len >= (int)sizeof(buffer)) {
        // 缓冲区不足，截断字符串
        len = sizeof(buffer) - 1;
        buffer[len] = '\0';
    }
    
    // 发送到串口
    if (len > 0) {
        USARTSend(g_usart_screen, (uint8_t *)buffer, (uint16_t)len, USART_TRANSFER_DMA);
    }
    
    return len;
}


/**
 * @brief printf 库底层单字符输出函数
 *        本工程使用 vsnprintf + 缓存 + DMA 方式发送，
 *        不直接调用 printf_()，此函数仅供链接器满足符号依赖。
 *        如需直接使用 printf() 打印，在此实现单字节 UART 发送。
 */
void _putchar(char c)
{
    (void)c;
    // 若需直接使用 printf("...") 而非 vsnprintf + USARTSend，
    // 在此处实现单字节 UART 发送：
    // while (!USARTIsReady(g_usart_screen));
    // USARTSend(g_usart_screen, (uint8_t*)&c, 1, USART_TRANSFER_BLOCKING);
}


void TJCUARTScreenInit(USART_Init_Config_s *init_cfg)
{
    g_usart_screen = USARTRegister(init_cfg);
}

/**
 * @brief 改变屏幕上指定文本框的内容
 * @param name 文本框的名称
 * @param value 要显示的字符串内容
 */
void HIM_change_string(const char *name, const char *value)
{
    if(g_usart_screen != NULL)
    {
        printf_DMA("%s.txt=\"%s\"\xff\xff\xff", name, value); 
    }
}
/**
 * @brief 改变屏幕上指定数值框的内容
 * @param name 数值框的名称
 * @param value 要显示的整数值
 */
void HIM_change_value(const char *name, int value)
{
    if(g_usart_screen != NULL)
    {
        printf_DMA("%s.val=%d\xff\xff\xff", name, value); 
    }
}
/**
 * @brief 请求屏幕上指定数值框的当前值
 * @param name 数值框的名称，.val后缀返回4字节整数值，小端模式，.txt后缀返回字符串值
 * 返回的值通过串口回调函数接收，用户需自行解析
 */
void HIM_print_value(const char *name)
{
    if(g_usart_screen != NULL)
    {
        printf_DMA("prints %s,0\xff\xff\xff", name); 
    }
}