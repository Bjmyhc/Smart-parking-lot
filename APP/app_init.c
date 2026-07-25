/****************************************************************************
 * 应用层初始化函数实现 - app_init.c
 * 
 * 功能描述:
 *   启动时一次性执行的初始化函数，归入业务层便于统一管理
 *   包含 BSP 外设初始化和 WiFi 连接初始化
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "bsp_ultrasonic.h"
#include "bsp_oled.h"
#include "bsp_qmc5883p.h"
#include "esp8266.h"
#include "onenet.h"
#include "device_config.h"
#include "app_global.h"

void BSP_Init(void)
{
    LED_Init();
    US_Init();
    SysTick_Init();
    Usart_Init();
    OLED_Init();
    QMC5883P_Init(&qmc5883p, QMC5883P_MODE_CONTINUOUS, QMC5883P_ODR_100HZ, QMC5883P_RNG_8G);
    Usart_Printf(USART_DEBUG, "All Bsp Init OK!\n");
}

void Wifi_Init(void)
{
    OLED_ShowCH(0, 0, (u8 *)"网络连接中...");
    Usart_Printf(USART_DEBUG, "ESP8266 Init...\n");
    ESP8266_Init();
    if (OneNet_DevLink() == ERROR)
        DelayXms(500);
    Usart_Printf(USART_DEBUG, "OneNET Connected!\n");
    OLED_Clear();
    OLED_ShowCH(0, 0, (u8 *)"网络连接成功!"); 
    DelayXms(500);
    OLED_Clear();
    OneNet_Subscribe(SubTopic, 1);
}
