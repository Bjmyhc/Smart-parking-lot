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
 *   - OLED显示屏 (I2C)
 * 
 * 通信协议:
 *   - MQTT协议连接OneNET云平台
 *   - 数据格式: JSON
 * 
 * 架构说明:
 *   采用时间戳非阻塞架构，所有模块在主循环中轮转执行
 *   模块间通过全局变量传递数据，避免阻塞调用
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
#include "bsp_ultrasonic.h"
#include "bsp_oled.h"

#include "esp8266.h"
#include "onenet.h"

/* 定时刷新间隔定义 */
#define DISPLAY_REFRESH_INTERVAL 500    /* 显示刷新间隔(ms) */
#define UPLOAD_INTERVAL          5000    /* 数据上传间隔(ms) */
#define ULTRASONIC_UPDATE_INTERVAL 500  /* 超声波读取间隔(ms) */

/* MQTT发布消息缓冲区 */
char PublishBuf[256];

/* 属性上报主题 */
const char PubTopic[] = "$sys/53BV12EYcY/park1/thing/property/post";

/* 属性设置订阅主题数组 */
const char *SubTopic[] = {"$sys/53BV12EYcY/park1/thing/property/set"};

/* ESP8266接收到的数据指针 */
unsigned char *pData = NULL;

/* 超声波距离值 */
uint16_t Distance = 0;

/* 车位状态: 0=空闲, 1=有车, 2=疑似僵尸车 */
uint8_t ParkStatus = 0;

/* 连续占用时间(秒) */
uint32_t OccupiedTime = 0;

/* 上次车位状态变化时间戳 */
uint32_t LastStatusChangeTick = 0;

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
    OLED_Init();          /* OLED显示屏初始化 */
    Usart_Printf(USART_DEBUG, "USART Init OK!\n");
}

/****************************************************************************
 * 函数名: US_Update
 * 功能:   更新超声波传感器数据
 * 参数:   无
 * 返回值: 无
 * 说明:   每500ms调用一次，更新全局变量Distance
 ****************************************************************************/
void US_Update(void)
{
    Distance = US_GetDistance();
}

/****************************************************************************
 * 函数名: ParkingStatus_Check
 * 功能:   检测车位状态
 * 参数:   无
 * 返回值: 无
 * 说明:   根据超声波距离判断车位是否有车
 *         距离 < 30cm 认为有车
 *         连续占用超过24小时认为疑似僵尸车
 ****************************************************************************/
void ParkingStatus_Check(void)
{
    uint8_t newStatus;
    
    if (Distance > 0 && Distance < 30)
    {
        newStatus = 1;
    }
    else
    {
        newStatus = 0;
    }
    
    if (newStatus != ParkStatus)
    {
        ParkStatus = newStatus;
        LastStatusChangeTick = Get_Tick();
        OccupiedTime = 0;
    }
    else if (ParkStatus == 1)
    {
        OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
        if (OccupiedTime > 24 * 60 * 60)
        {
            ParkStatus = 2;
        }
    }
}

/****************************************************************************
 * 函数名: OLED_ShowMain
 * 功能:   OLED主界面显示
 * 参数:   无
 * 返回值: 无
 * 说明:   显示超声波距离和车位状态
 ****************************************************************************/
void OLED_ShowMain(void)
{
    OLED_ShowCH(0, 0, (u8 *)"超声波数据");
    
    OLED_ShowCH(0, 2, (u8 *)"距离: ");
    OLED_ShowNum(48, 2, Distance, 3, 1);
    OLED_ShowCH(72, 2, (u8 *)"cm");
    
    OLED_ShowCH(0, 4, (u8 *)"状态: ");
    switch (ParkStatus)
    {
        case 0:
            OLED_ShowCH(48, 4, (u8 *)"空闲");
            break;
        case 1:
            OLED_ShowCH(48, 4, (u8 *)"有车");
            break;
        case 2:
            OLED_ShowCH(48, 4, (u8 *)"僵尸车");
            break;
        default:
            OLED_ShowCH(48, 4, (u8 *)"未知");
            break;
    }
    
    if (ParkStatus != 0)
    {
        OLED_ShowCH(0, 6, (u8 *)"占用: ");
        OLED_ShowNum(48, 6, OccupiedTime, 5, 1);
        OLED_ShowCH(96, 6, (u8 *)"秒");
    }
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
            (unsigned int)Get_Tick(),
            ParkStatus,
            0,
            Distance,
            OccupiedTime);
}

/****************************************************************************
 * 函数名: main
 * 功能:   主程序入口
 * 参数:   无
 * 返回值: 无
 * 
 * 主循环流程(时间戳非阻塞架构):
 *   1. 超声波数据更新 (500ms)
 *   2. 车位状态检测 + OLED显示刷新 (500ms)
 *   3. 数据上报OneNET (5000ms)
 *   4. 接收并处理平台下发指令 (每轮)
 ****************************************************************************/
int main(void)
{
    uint32_t LastUploadTick = 0;        /* 上次上报时间戳 */
    uint32_t LastDisplayTick = 0;       /* 上次显示刷新时间戳 */
    uint32_t LastUltrasonicTick = 0;    /* 上次超声波读取时间戳 */

    /* 初始化所有板级外设 */
    BSP_Init();

    /* OLED显示: 网络连接中 */
    OLED_ShowCH(0, 0, (u8 *)"网络连接中");
    

    /* 初始化ESP8266 WiFi模块 */
    Usart_Printf(USART_DEBUG, "ESP8266 Init...\n");
    ESP8266_Init();

    /* 连接OneNET云平台 */
    if (OneNet_DevLink() == ERROR)
    {
        DelayXms(500);
    }

    /* 连接成功提示 */
    Usart_Printf(USART_DEBUG, "OneNET Connected!\n");

    /* OLED显示: 网络连接成功 */
    OLED_Clear();
    OLED_ShowCH(0, 0, (u8 *)"网络连接成功");
    
    DelayXms(1000);
    
    /* 清空OLED，准备进入主界面 */
    OLED_Clear();

    /* 订阅属性设置主题 */
    OneNet_Subscribe(SubTopic, 1);

    /* 主循环 - 时间戳非阻塞架构 */
    while (1)
    {
        /* 每500ms更新超声波传感器数据 */
        if (Get_Tick() - LastUltrasonicTick >= ULTRASONIC_UPDATE_INTERVAL)
        {
            Distance = US_GetDistance();
            LastUltrasonicTick = Get_Tick();
        }

        /* 每500ms刷新OLED显示 */
        if (Get_Tick() - LastDisplayTick >= DISPLAY_REFRESH_INTERVAL)
        {
            //OLED_Clear();
            OLED_ShowCH(0, 0, (u8 *)"网络已连接");
            OLED_ShowCH(0, 2, (u8 *)"超声波数据");
            OLED_Printf(0, 4, "距离: %.3d cm", Distance);
            //OLED_Refresh();
            LastDisplayTick = Get_Tick();
        }

        /* 每5秒上传一次数据到平台 */
        if (Get_Tick() - LastUploadTick >= UPLOAD_INTERVAL)
        {
            GenerateParkingData();
            OneNet_Publish(PubTopic, PublishBuf);
            ESP8266_Clear();
            LastUploadTick = Get_Tick();
            Usart_Printf(USART_DEBUG, "Data uploaded!\n");
        }

        /* 检查ESP8266是否收到数据(超时20ms) */
        pData = ESP8266_GetIPD(20);

        /* 处理平台下发的指令 */
        if (pData != NULL)
        {
            OneNet_RevPro(pData);
            ESP8266_Clear();
        }
    }
}
