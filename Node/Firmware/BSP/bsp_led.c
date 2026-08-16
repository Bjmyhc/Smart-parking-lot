/****************************************************************************
 * LED驱动 - bsp_led.c
 * 
 * 功能描述:
 *   实现LED指示灯的初始化和开关控制
 *   用于系统状态指示、错误提示等
 * 
 * 硬件配置:
 *   - LED引脚定义在bsp_led.h中
 *   - 默认配置为推挽输出,低电平点亮
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "bsp_led.h"

static uint8_t ledState = 0;  /* LED状态: 0=熄灭, 1=点亮 */

/****************************************************************************
 * 函数名: LED_Init
 * 功能:   初始化LED指示灯
 * 参数:   无
 * 返回值: 无
 * 说明:   配置LED引脚为推挽输出,默认熄灭(高电平)
 ****************************************************************************/
void LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(LED_GPIO_CLK, ENABLE);

    GPIO_InitStructure.GPIO_Pin = LED_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

    GPIO_Init(LED_GPIO_PORT, &GPIO_InitStructure);

    GPIO_SetBits(LED_GPIO_PORT, LED_GPIO_PIN);
}

/****************************************************************************
 * 函数名: LED_ON
 * 功能:   点亮LED
 * 参数:   无
 * 返回值: 无
 * 说明:   通过拉低引脚电平点亮LED
 ****************************************************************************/
void LED_ON(void)
{
    GPIO_ResetBits(LED_GPIO_PORT, LED_GPIO_PIN);
    ledState = 1;
}

/****************************************************************************
 * 函数名: LED_OFF
 * 功能:   熄灭LED
 * 参数:   无
 * 返回值: 无
 * 说明:   通过拉高引脚电平熄灭LED
 ****************************************************************************/
void LED_OFF(void)
{
    GPIO_SetBits(LED_GPIO_PORT, LED_GPIO_PIN);
    ledState = 0;
}

/****************************************************************************
 * 函数名: LED_GetState
 * 功能:   获取LED状态
 * 参数:   无
 * 返回值: 0=熄灭, 1=点亮
 ****************************************************************************/
uint8_t LED_GetState(void)
{
    return ledState;
}
