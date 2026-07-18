#include "bsp_led.h"


void LED_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	RCC_APB2PeriphClockCmd(LED_GPIO_CLK,ENABLE);
	
	GPIO_InitStructure.GPIO_Pin = LED_GPIO_PIN;

	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;

	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

	GPIO_Init(LED_GPIO_PORT,&GPIO_InitStructure);

	GPIO_SetBits(LED_GPIO_PORT,LED_GPIO_PIN);	
}

void LED_ON(void)
{
		GPIO_ResetBits(LED_GPIO_PORT,LED_GPIO_PIN);	
}
void LED_OFF(void)
{

		GPIO_SetBits(LED_GPIO_PORT,LED_GPIO_PIN);		
}