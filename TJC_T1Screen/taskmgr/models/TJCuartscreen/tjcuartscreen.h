#ifndef TJCUARTSCREEN_H
#define TJCUARTSCREEN_H
#include "bsp_usart.h"

void TJCUARTScreenInit(USART_Init_Config_s *init_cfg);
void HIM_change_string(const char *name, const char *value);
void HIM_change_value(const char *name, int value);
void HIM_print_value(const char *name);


#endif

