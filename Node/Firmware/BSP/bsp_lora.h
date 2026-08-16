/****************************************************************************
 * LoRa 节点通信驱动 - bsp_lora.h
 *
 * 功能描述:
 *   升级现有 LoRa 透传模块为定点传输驱动
 *   实现节点与网关之间的双向通信:
 *   - 响应网关轮询(证书查询/数据查询)
 *   - 发送传感器数据(结构体打包)
 *   - 接收下行控制命令(LED使能等)
 *
 * 硬件配置:
 *   USART2 (PA2/TX, PA3/RX) - LoRa 模块通信串口
 *   定点传输模式: [AddrH][AddrL][CH] + 数据
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-07
 ****************************************************************************/

#ifndef __LORA_NODE_H
#define __LORA_NODE_H

#include <stdint.h>
#include "stm32f10x.h"

/* ==================== 节点地址配置 ==================== */
/* 每个节点编译时固定地址, 网关地址固定为 0x0000 */
#ifndef LORA_NODE_ADDR
#define LORA_NODE_ADDR      0x0001      /* 节点1地址, 其他节点修改此宏 */
#endif

#define LORA_GATEWAY_ADDR   0x0000      /* 网关地址 */
#define LORA_CHANNEL        0x00        /* 信道(0), DX-LR22模块: 00=433.15MHz */
#define LORA_BAUD           9600        /* LoRa 串口波特率，与网关端 SoftwareSerial 一致 */

/* ==================== OneNET 子设备证书配置 ====================
 * 节点通过 LoRa 上报证书给网关, 网关代为上线 OneNET (网关+子设备模式)
 * 注意:
 *   1. 产品ID 是"子设备所在产品"的ID, 与网关产品可能不同 (见下方配置)
 *   2. 设备名称必须是 OneNET 平台已创建、且已绑定到网关拓扑的子设备名
 *   3. 不同节点烧录时改 LORA_SUB_DEVICE_NAME (节点1=Park001, 节点2=Park002...) */
#ifndef LORA_SUB_PRODUCT_KEY
#define LORA_SUB_PRODUCT_KEY   "04kjwU9TC7"   /* 子设备产品ID */
#endif
#ifndef LORA_SUB_DEVICE_NAME
#define LORA_SUB_DEVICE_NAME   "Park001"      /* 子设备设备名 */
#endif

/* ==================== 帧头定义 ==================== */
/* 数据帧头字节(子设备→网关), 借鉴参考项目帧头方案 */
#define LORA_FRAME_CERT     0xA1        /* 证书数据帧 */
#define LORA_FRAME_DATA     0xB1        /* 传感器数据帧 */
#define LORA_FRAME_ACK      0xC1        /* 命令执行确认帧 */
#define LORA_FRAME_OTA_OK   0xD1        /* OTA 接收成功 */
#define LORA_FRAME_OTA_RETRY 0xE1       /* OTA 要求重发 */

/* ==================== 数据结构 ==================== */

/* 节点传感器数据(与网关端 NodeData_t 一致) */
#pragma pack(push, 1)
typedef struct {
    uint8_t  ParkStatus;      /* 0=空闲, 1=有车, 2=僵尸车 */
    uint8_t  GeoMagnetic;     /* 0/1 */
    uint16_t Ultrasonic;      /* 距离(cm) */
    uint32_t OccupiedTime;    /* 占用时长(秒) */
    uint8_t  LED;             /* LED 当前状态 0/1 */
    uint8_t  LedEnable;       /* LED 使能 0/1 */
    uint16_t FwVersion;       /* 固件版本(APP_VERSION, 如 0x0203 = v2.3) */
} NodeData_t;
#pragma pack(pop)

/* 节点证书(首次上线时发送给网关, 网关代为上线 OneNET) */
#pragma pack(push, 1)
typedef struct {
    uint8_t  valid;           /* 0=未配置, 1=有效 */
    char     ProductKey[12];  /* OneNET 产品ID */
    char     DeviceName[33];  /* OneNET 设备名称 */
    char     AccessKey[33];   /* OneNET 设备密钥 */
} NodeCert_t;
#pragma pack(pop)

/* ==================== 命令回调 ==================== */
/* 网关下行命令回调函数类型
 * cmd:  命令名称(如 "AT+DATA", "AT+CER", "AT+LedEnable")
 * value: 命令参数值(如 "0", "1", 无参数时为NULL) */
typedef void (*LoRaCmdCallback)(const char *cmd, const char *value);

/* ==================== 接口函数 ==================== */

/****************************************************************************
 * 初始化 LoRa 模块(定点传输模式)
 * - 初始化 USART2
 * - 配置 LoRa 模块地址和信道
 * - 清空接收缓冲
 ****************************************************************************/
void LoRa_Node_Init(void);

/****************************************************************************
 * 发送证书给网关(首次上线/网关查询时调用)
 * cert: 节点证书结构体指针
 ****************************************************************************/
void LoRa_Node_SendCert(const NodeCert_t *cert);

/****************************************************************************
 * 发送传感器数据给网关(网关查询数据时调用)
 * data: 节点数据结构体指针
 ****************************************************************************/
void LoRa_Node_SendData(const NodeData_t *data);

/****************************************************************************
 * 发送命令执行确认给网关
 * cmd: 原始命令字符串
 ****************************************************************************/
void LoRa_Node_SendAck(const char *cmd);

/****************************************************************************
 * 轮询接收网关命令(非阻塞)
 * cb: 命令回调函数, 收到命令时调用
 * 返回值: 1=收到并处理了命令, 0=无命令
 * 说明: 网关通过定点传输发送 AT 命令(如 "AT+DATA\r\n"),
 *       节点解析后回调, 回调中可调用 SendData/SendCert 发送响应
 ****************************************************************************/
uint8_t LoRa_Node_Poll(LoRaCmdCallback cb);

/****************************************************************************
 * 是否已被网关轮询到(在线状态)
 * 返回值: 1=在线(曾被网关查询过), 0=离线
 ****************************************************************************/
uint8_t LoRa_Node_IsOnline(void);

#endif /* __LORA_NODE_H */
