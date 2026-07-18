#ifndef BSP_LED_H
#define BSP_LED_H
#include "stm32f10x.h"



#define LED_GPIO_PORT GPIOA
#define LED_GPIO_CLK  RCC_APB2Periph_GPIOA
#define LED_GPIO_PIN  GPIO_Pin_0


void LED_Init(void);
void LED_ON(void);
void LED_OFF(void);
#endif
