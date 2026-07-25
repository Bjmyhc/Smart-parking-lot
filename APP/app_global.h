/****************************************************************************
 * 应用层全局变量声明 - app_global.h
 * 
 * 功能描述:
 *   所有跨文件共享的全局变量在此声明为 extern，实际定义在 app_tasks.c 中
 *   包括传感器数据、车位状态、LED控制、WiFi通信相关变量
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#ifndef __APP_GLOBAL_H
#define __APP_GLOBAL_H

#include <stdint.h>
#include "bsp_qmc5883p.h"

/* ==================== 传感器数据 ==================== */

/* 超声波距离值 (cm) */
extern uint16_t Distance;

/* QMC5883P 磁力计设备 */
extern QMC5883P_Device_t qmc5883p;

/* 地磁检测车辆存在标志 (0=无车, 1=有车) */
extern uint8_t MagCarPresent;

/* ==================== 车位状态 ==================== */

typedef enum {
    PARK_IDLE = 0,      /* 空闲 */
    PARK_OCCUPIED = 1,  /* 有车 */
    PARK_ZOMBIE = 2     /* 疑似僵尸车 */
} ParkStatus_t;

extern ParkStatus_t ParkStatus;
extern uint32_t OccupiedTime;           /* 连续占用时间(秒) */
extern uint32_t LastStatusChangeTick;   /* 上次车位状态变化时间戳 */

/* ==================== LED控制 ==================== */

/* LED使能标志: 0=云端禁用, 1=云端启用(本地自动控制) */
extern uint8_t LEDEnable;

/* ==================== WiFi通信 ==================== */

extern char PublishBuf[256];            /* MQTT发布消息缓冲区 */
extern const char PubTopic[];           /* 属性上报主题 */
extern const char *SubTopic[];          /* 属性设置订阅主题数组 */
extern unsigned char *pData;            /* ESP8266接收到的数据指针 */

#endif /* __APP_GLOBAL_H */
