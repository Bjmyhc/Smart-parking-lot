/****************************************************************************
 * 应用层周期任务函数实现 - app_tasks.c
 * 
 * 功能描述:
 *   包含全局变量定义和主循环中调用的所有业务任务函数
 *   超声波滤波、地磁检测、车位状态判断、OLED显示、WiFi上传
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#include <stdio.h>
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

/* ==================== 宏定义 ==================== */

#define UPLOAD_INTERVAL         2000    /* WiFi数据上报周期(ms) */
#define US_UPDATE_INTERVAL      100     /* 超声波采集间隔(ms) */
#define US_SHIFT                2       /* EMA滤波系数 alpha=1/4 */
#define US_MIN_VALID            2       /* 超声波最小有效距离(cm) */
#define US_MAX_VALID            400     /* 超声波最大有效距离(cm) */
#define QMC_UPDATE_INTERVAL     100      /* 地磁采集间隔(ms) */
#define MAG_DEBOUNCE_CNT        2       /* 地磁消抖连续采样次数 */
#define MAG_Z_SQ_THRESH         2.0f    /* 地磁Z轴平方阈值 */
#define PARK_CHECK_INTERVAL     200     /* 车位状态检测周期(ms) */
#define DIST_THRESHOLD_CM       10     /* 超声波判定有车的距离阈值(cm) */
#define OLED_UPDATE_INTERVAL    250     /* OLED刷新周期(ms) */

/* ==================== 全局变量定义 ==================== */

char PublishBuf[256];
const char PubTopic[] = TOPIC_PROPERTY_POST;
const char *SubTopic[] = {TOPIC_PROPERTY_SET};
unsigned char *pData = NULL;

uint16_t Distance = 0;
ParkStatus_t ParkStatus = PARK_IDLE;
uint32_t OccupiedTime = 0;
uint32_t LastStatusChangeTick = 0;
uint8_t LEDEnable = 1;
QMC5883P_Device_t qmc5883p;
uint8_t MagCarPresent = 0;

/****************************************************************************
 * 函数名: US_Task
 * 功能:   超声波传感器业务任务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   无效值剔除 + 一阶低通滤波 (EMA)
 *         alpha = 1/4, 用移位实现全整数运算, 无需浮点
 *         超时/过近/过远的异常值直接丢弃, 保持上次有效值
 *         直接修改全局变量 Distance
 ****************************************************************************/
void US_Task(void)
{
    static uint32_t lastUpdateTick = 0;
    static uint16_t smoothDist = 0;
    static uint8_t firstRun = 1;

    if (Get_Tick() - lastUpdateTick >= US_UPDATE_INTERVAL)
    {
        uint16_t raw = US_GetDistance();
		
        /* 无效值剔除: 超时(=0), 过近, 过远 -> 保持上次有效值 */
        if (raw < US_MIN_VALID || raw > US_MAX_VALID)
            raw = smoothDist;

        /* 首次运行直接取原始值 */
        if (firstRun)
        {
            smoothDist = raw;
            firstRun = 0;
        }
        else
        {
            /* EMA: output = (output * (2^N - 1) + input) >> N
             *      = output * 0.75 + input * 0.25   (N=2) */
            smoothDist = (uint16_t)(((uint32_t)smoothDist * ((1 << US_SHIFT) - 1) + raw) >> US_SHIFT);
        }

        Distance = smoothDist;
        lastUpdateTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: QMC_Task
 * 功能:   QMC5883P 地磁车辆检测任务
 * 参数:   无
 * 返回值: 无
 * 说明:   每200ms采集一次Z轴磁场数据
 *         检测逻辑: Z轴平方 > 5 判定为有车
 *         连续采样3次均满足条件才确认有车，防止瞬时抖动误判
 ****************************************************************************/
void QMC_Task(void)
{
    static uint32_t lastUpdateTick = 0;
    static uint8_t debounceCnt = 0;

    if (Get_Tick() - lastUpdateTick >= QMC_UPDATE_INTERVAL)
    {
        if (QMC5883P_Update(&qmc5883p) == QMC5883P_OK)
        {
            float z = QMC5883P_GetZ(&qmc5883p);
            float zSq = z * z;

            /* 检测逻辑: Z轴平方大于阈值 */
            uint8_t triggered = (zSq > MAG_Z_SQ_THRESH);

            /* --- 消抖: 连续3次确认 --- */
            if (triggered)
            {
                if (debounceCnt < MAG_DEBOUNCE_CNT)
                    debounceCnt++;
                if (debounceCnt >= MAG_DEBOUNCE_CNT)
                    MagCarPresent = 1;
            }
            else
            {
                if (debounceCnt > 0)
                    debounceCnt--;
                if (debounceCnt == 0)
                    MagCarPresent = 0;
            }

            /* 调试打印 */
            Usart_Printf(USART_DEBUG, "QMC: Z=%.2f, zSq=%.1f, cnt=%d, car=%d\r\n",z, zSq, debounceCnt, MagCarPresent);
        }
        else
        {
            Usart_Printf(USART_DEBUG, "QMC: Update FAIL!\r\n");
        }

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
    static uint32_t lastCheckTick = 0;
    
    if (Get_Tick() - lastCheckTick >= PARK_CHECK_INTERVAL)
    {
        uint8_t carPresent = (Distance > 0 && Distance < DIST_THRESHOLD_CM) && MagCarPresent;
        
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
                    if (OccupiedTime > 5)               /* 演示用：10秒变僵尸车 */
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
    static uint32_t lastUpdateTick = 0;
    
    if (Get_Tick() - lastUpdateTick >= OLED_UPDATE_INTERVAL)
    {
        OLED_ShowCH(0, 0, (u8 *)"网络已连接");
        OLED_Printf(0, 2, "地磁: %d", MagCarPresent);
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
                OLED_Printf(48, 6, "僵尸车   ");
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
            MagCarPresent,
            Distance,
            OccupiedTime,
            LED_GetState() ? "true" : "false",
            LEDEnable ? "true" : "false");
}

/****************************************************************************
 * 函数名: Wifi_Task
 * 功能:   WiFi通信业务任务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   每5000ms上报一次数据到OneNET平台
 *         每次主循环检查ESP8266是否收到下行数据并处理
 ****************************************************************************/
void Wifi_Task(void)
{
    static uint32_t lastUploadTick = 0;

    /* 每5秒上传一次数据到平台 */
    if (Get_Tick() - lastUploadTick >= UPLOAD_INTERVAL)
    {
        GenerateParkingData();
        OneNet_Publish(PubTopic, PublishBuf);
        ESP8266_Clear();
        lastUploadTick = Get_Tick();
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


