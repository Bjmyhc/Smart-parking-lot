#ifndef BSP_USART_H
#define BSP_USART_H

#include "stm32f10x.h"
#include <stdio.h>




#define USART_DEBUG		USART1		//调试打印使用的串口号


void Usart1_Init(unsigned int baud);

void Usart2_Init(unsigned int baud);

void Usart_SendString(USART_TypeDef *USARTx, unsigned char *str, unsigned short len);

void Usart_Printf(USART_TypeDef *USARTx, char *fmt,...);

void Usart_Init(void);
#endif
