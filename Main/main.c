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
#include "device_config.h"

/* 定时刷新间隔定义 */
#define UPLOAD_INTERVAL          5000    /* 数据上传间隔(ms) */

/* MQTT发布消息缓冲区 */
char PublishBuf[256];

/* 属性上报主题 */
const char PubTopic[] = TOPIC_PROPERTY_POST;

/* 属性设置订阅主题数组 */
const char *SubTopic[] = {TOPIC_PROPERTY_SET};

/* ESP8266接收到的数据指针 */
unsigned char *pData = NULL;

/* 超声波距离值 */
uint16_t Distance = 0;

/* 车位状态枚举 */
enum { PARK_IDLE = 0, PARK_OCCUPIED = 1, PARK_ZOMBIE = 2 };
uint8_t ParkStatus = PARK_IDLE;

/* 连续占用时间(秒) */
uint32_t OccupiedTime = 0;

/* 上次车位状态变化时间戳 */
uint32_t LastStatusChangeTick = 0;

/* LED使能标志: 0=云端禁用, 1=云端启用(本地自动控制) */
uint8_t LEDEnable = 1;

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
 * 函数名: US_Task
 * 功能:   超声波传感器业务任务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   包含非阻塞计时、移动平均滤波和距离上限处理
 *         超过400cm时设置为380cm
 *         内部每500ms自动更新一次数据
 *         直接修改全局变量Distance
 ****************************************************************************/
void US_Task(void)
{
    #define US_FILTER_SIZE 10
    #define US_UPDATE_INTERVAL 100
    
    static uint16_t usBuffer[US_FILTER_SIZE] = {0};
    static uint8_t usBufferIndex = 0;
    static uint32_t lastUpdateTick = 0;
    
    if (Get_Tick() - lastUpdateTick >= US_UPDATE_INTERVAL)
    {
        uint16_t rawDistance = US_GetDistance();
        
        if (rawDistance > 400)
        {
            rawDistance = 380;
        }
        
        usBuffer[usBufferIndex++] = rawDistance;
        if (usBufferIndex >= US_FILTER_SIZE)
        {
            usBufferIndex = 0;
        }
        
        uint32_t sum = 0;
        for (uint8_t i = 0; i < US_FILTER_SIZE; i++)
        {
            sum += usBuffer[i];
        }
        
        Distance = (uint16_t)(sum / US_FILTER_SIZE);
        lastUpdateTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: ParkingStatus_Check
 * 功能:   检测车位状态
 * 参数:   无
 * 返回值: 无
 * 说明:   根据超声波距离判断车位是否有车
 *         距离 < 100cm 认为有车
 *         连续占用超过24小时认为疑似僵尸车
 ****************************************************************************/
void ParkingStatus_Check(void)
{
    #define PARK_CHECK_INTERVAL 500
    #define DIST_THRESHOLD_CM 100
    static uint32_t lastCheckTick = 0;
    
    if (Get_Tick() - lastCheckTick >= PARK_CHECK_INTERVAL)
    {
        uint8_t carPresent = (Distance > 0 && Distance < DIST_THRESHOLD_CM) ? 1 : 0;
        
        switch (ParkStatus)
        {
            case PARK_IDLE:
                if (carPresent)
                {
                    ParkStatus = PARK_OCCUPIED;         /* 车来了 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                }
                break;
                
            case PARK_OCCUPIED:
                if (!carPresent)
                {
                    ParkStatus = PARK_IDLE;             /* 车离开了 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                }
                else
                {
                    OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
                    //if (OccupiedTime > 24 * 60 * 60)
                    if (OccupiedTime > 10)               /* 演示用：10秒变僵尸车 */
                    {
                        ParkStatus = PARK_ZOMBIE;

                    }
                }
                break;
                
            case PARK_ZOMBIE:
                OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
                if (!carPresent)
                {
                    ParkStatus = PARK_IDLE;             /* 车离开了 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                }
                break;
        }
        
        lastCheckTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: LED_Task
 * 功能:   LED控制任务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   云端使能(LEDEnable=1)时，本地根据状态自动控制LED
 *         僵尸车→点亮, 其他→熄灭
 *         云端禁用(LEDEnable=0)时，强制熄灭LED
 ****************************************************************************/
void LED_Task(void)
{
    if (LEDEnable)
    {
        if (ParkStatus == PARK_ZOMBIE)
            LED_ON();
        else
            LED_OFF();
    }
    else
    {
        LED_OFF();
    }
}

/****************************************************************************
 * 函数名: OLED_Task
 * 功能:   OLED显示任务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   包含非阻塞计时，每500ms刷新一次OLED显示
 *         显示网络状态、超声波数据、距离值和车位状态
 *         距离使用OLED_Printf格式化输出
 ****************************************************************************/
void OLED_Task(void)
{
    #define OLED_UPDATE_INTERVAL 500
    static uint32_t lastUpdateTick = 0;
    
    if (Get_Tick() - lastUpdateTick >= OLED_UPDATE_INTERVAL)
    {
        OLED_ShowCH(0, 0, (u8 *)"网络已连接");
        //OLED_ShowCH(0, 2, (u8 *)"超声波数据");
        OLED_Printf(0, 4, "距离: %.3d cm", Distance);
        OLED_ShowCH(0, 6, (u8 *)"状态: ");
        switch (ParkStatus)
        {
            case PARK_IDLE:
                OLED_Printf(48, 6, "空闲     ");
                break;
            case PARK_OCCUPIED:
                OLED_Printf(48, 6, "有车 %3ds", OccupiedTime);
                break;
            case PARK_ZOMBIE:
                OLED_Printf(48, 6, "僵尸车   ", OccupiedTime);
                break;
            default:
                OLED_Printf(48, 6, "未知     ");
                break;
        }
        
        lastUpdateTick = Get_Tick();
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
 *   LED:           LED实际状态 (true=点亮, false=熄灭, 布尔类型)
 *   LedEnable:     云端使能标志 (true=启用, false=禁用, 布尔类型)
 ****************************************************************************/
void GenerateParkingData(void)
{
    sprintf(PublishBuf,
            "{\"id\":\"%u\",\"params\":{"
            "\"ParkStatus\":{\"value\":%d},"
            "\"GeoMagnetic\":{\"value\":%d},"
            "\"Ultrasonic\":{\"value\":%d},"
            "\"OccupiedTime\":{\"value\":%d},"
             "\"LED\":{\"value\":%s},"
             "\"LedEnable\":{\"value\":%s}}}",
            (unsigned int)Get_Tick(),
            ParkStatus,
            0,
            Distance,
            OccupiedTime,
            LED_GetState() ? "true" : "false",
            LEDEnable ? "true" : "false");
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
        /* 更新超声波传感器数据(含滤波) */
        US_Task();
		
		/* 检查车位状态 */
        ParkingStatus_Check();

        /* LED控制(GPIO实际控制) */
        LED_Task();

        /* 更新OLED显示 */
        OLED_Task();

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
