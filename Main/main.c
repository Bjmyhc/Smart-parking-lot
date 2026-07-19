/****************************************************************************
 * 智能停车场系统 - 主程序文件
 * 
 * 功能描述:
 *   基于STM32 + ESP8266实现的智能停车场车位检测系统
 *   通过地磁传感器和超声波传感器融合检测车位状态
 *   将数据上传至OneNET云平台
 * 
 * 硬件配置:
 *   - STM32F103C8T6 主控芯片
 *   - ESP8266 WiFi模块 (USART2)
 *   - 地磁传感器 (ADC采样)
 *   - 超声波传感器 (TIM1输入捕获)
 *   - LED指示灯 (GPIO)
 * 
 * 通信协议:
 *   - MQTT协议连接OneNET云平台
 *   - 数据格式: JSON
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "esp8266.h"
#include "onenet.h"

/* 数据上报间隔(ms) */
#define UPLOAD_INTERVAL 5000
/* MQTT发布消息缓冲区 */
char PublishBuf[256];
/* 属性上报主题 */
const char PubTopic[] = "$sys/53BV12EYcY/park1/thing/property/post";
/* 属性设置订阅主题数组 */
const char *SubTopic[] = {"$sys/53BV12EYcY/park1/thing/property/set"};
/* ESP8266接收到的数据指针 */
unsigned char *pData = NULL;

/****************************************************************************
 * 函数名: BSP_Init
 * 功能:   初始化所有板级外设
 * 参数:   无
 * 返回值: 无
 ****************************************************************************/
void BSP_Init(void)
{
    LED_Init();           /* LED指示灯初始化 */
    US_Init();            /* 超声波传感器初始化 */
    SysTick_Init();       /* 系统滴答定时器初始化 */
    Usart_Init();         /* 串口初始化(USART1调试, USART2连接ESP8266) */
    Usart_Printf(USART1, "USART Init OK!\n");
}

/****************************************************************************
 * 函数名: GenerateParkingData
 * 功能:   生成停车场数据上报JSON
 * 参数:   无
 * 返回值: 无
 * 
 * 上报数据结构:
 *   ParkStatus:    车位状态 (0=空闲, 1=有车, 2=疑似僵尸车)
 *   GeoMagnetic:   地磁传感器采样值
 *   Ultrasonic:    超声波距离值(cm)
 *   OccupiedTime:  连续占用时间(秒)
 ****************************************************************************/
void GenerateParkingData(void)
{
    sprintf(PublishBuf,
            "{\"id\":\"%u\",\"params\":{"
            "\"ParkStatus\":{\"value\":%d},"
            "\"GeoMagnetic\":{\"value\":%d},"
            "\"Ultrasonic\":{\"value\":%d},"
            "\"OccupiedTime\":{\"value\":%d}}}",
            (unsigned int)Get_Tick(),  /* 请求ID,使用系统滴答值 */
            0,                          /* 车位状态(待实现) */
            0,                          /* 地磁值(待实现) */
            0,                          /* 超声波距离(待实现) */
            0);                         /* 占用时长(待实现) */
}

/****************************************************************************
 * 函数名: main
 * 功能:   主程序入口
 * 参数:   无
 * 返回值: 无
 * 
 * 主循环流程:
 *   1. 初始化外设和网络连接
 *   2. 定时(5秒)上报停车场数据
 *   3. 接收并处理平台下发的指令
 ****************************************************************************/
int main(void)
{
    uint32_t LastUploadTick = 0;  /* 上次上报时间戳 */

    /* 初始化所有板级外设 */
    BSP_Init();

    /* 初始化ESP8266 WiFi模块 */
    Usart_Printf(USART_DEBUG, "ESP8266 Init...\n");
    ESP8266_Init();

    /* 连接OneNET云平台 */
    if (OneNet_DevLink() == ERROR)
    {
        DelayXms(500);  /* 连接失败延时等待 */
    }

    /* 连接成功提示 */
    Usart_Printf(USART_DEBUG, "OneNET Connected!\n");

    /* 订阅属性设置主题 */
    OneNet_Subscribe(SubTopic, 1);

    /* 主循环 */
    while (1)
    {
        /* 定时上报数据 */
        if (Get_Tick() - LastUploadTick >= UPLOAD_INTERVAL)
        {
            GenerateParkingData();       /* 生成上报数据 */
            OneNet_Publish(PubTopic, PublishBuf);  /* 发布到OneNET */
            ESP8266_Clear();            /* 清除ESP8266接收缓冲区 */
            LastUploadTick = Get_Tick(); /* 更新上次上报时间 */
        }

        /* 检查ESP8266是否收到数据 */
        pData = ESP8266_GetIPD(20);

        /* 处理平台下发的指令 */
        if (pData != NULL)
        {
            OneNet_RevPro(pData);       /* 解析并处理MQTT消息 */
            ESP8266_Clear();            /* 清除接收缓冲区 */
        }
    }
}