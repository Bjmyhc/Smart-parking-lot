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
#include <stddef.h>   /* offsetof (CRC 计算用) */
#include "stm32f10x.h"

/* ==================== 节点地址配置 ==================== */
/* 每个节点编译时固定地址, 网关地址固定为 0x0000 */
#ifndef LORA_NODE_ADDR
#define LORA_NODE_ADDR      0x0001      /* 节点1地址, 其他节点修改此宏 */
#endif

#define LORA_GATEWAY_ADDR   0x0000      /* 网关地址 */
#define LORA_CHANNEL        0x00        /* 信道(0), DX-LR22模块: 00=433.15MHz */
#define LORA_BAUD           9600        /* LoRa 串口波特率，与网关端 SoftwareSerial 一致 */

/* ==================== LoRa 模块 AUX 引脚 ====================
 * AUX 是模块输出, 反映模块忙闲状态:
 *   高=数据发送中/接收中/模式切换中(忙)
 *   低=发送完成/接收完成/切换完成(闲)
 * 节点端接 STM32F103 的 PA11 (该脚默认 USART1_CTS / USB_DM, 但本项目
 * 未用 USART1 硬件流控也未用 USB, 故空闲可用作普通 GPIO 输入).
 * 发送前后查 AUX 状态, 判定模块是否收到/发完, 异常时日志报 FAIL.
 * 与网关端 hw_cfg.h 的 LORA_AUX_PIN/LORA_AUX_WAIT_MS 对称配置 */
#define LORA_AUX_PORT       GPIOA
#define LORA_AUX_PIN        GPIO_Pin_11   /* PA11 */
#define LORA_AUX_WAIT_MS    50UL          /* 等 AUX 变化的超时(ms) */

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

/* ==================== 协议版本 ==================== */
#define LORA_PROTO_VERSION    2   /* v2: 加 seq + crc16 字段 */

/* ==================== CRC16/MODBUS (工业标准, 多项式 0xA001) ====================
 * 覆盖范围: 整个结构体除 crc16 字段外的所有字节
 * 漏检概率 ~ 1/65536, 对 19-32 字节短帧完全够用 (LoRaWAN 也用 CRC16)
 * 两端共用此函数, static 关键字避免多文件 link 冲突 */
static inline uint16_t lora_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;
    for (i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i];
        for (b = 0; b < 8; b++)
        {
            if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else         crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* ==================== 数据结构 ==================== */

/* 节点传感器数据(v2: 19 字节, 加 seq + crc16)
 * ⭐ v2 协议: 末尾追加 seq(1B) + crc16(2B), 16B → 19B
 *   - seq: 节点每次发送 ++, 0..255 循环 (网关端可记录检测重复/丢包)
 *   - crc16: CRC16/MODBUS, 覆盖 [结构体首, offsetof(crc16)) 字节
 * 节点端发送前填, 网关端接收校验, 不通过直接丢弃 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  ParkStatus;      /* 0=空闲, 1=有车, 2=僵尸车 */
    uint8_t  GeoMagnetic;     /* 0/1 */
    uint16_t Ultrasonic;      /* 距离(cm) */
    uint32_t OccupiedTime;    /* 占用时长(秒) */
    uint8_t  LED;             /* LED 当前状态 0/1 */
    uint8_t  LedEnable;       /* LED 使能 0/1 */
    uint32_t ZombieThreshold; /* ⭐ 僵尸车判定阈值(秒), 当前生效值, 上报给平台观看 */
    uint16_t SensorDistanceCm; /* ⭐ 超声波判定距离阈值(cm), 当前生效值 */
    /* === v2 协议新增字段 (放末尾, 兼容前向布局) === */
    uint8_t  seq;             /* 帧序列号, 节点每次发送 ++, 0..255 循环 */
    uint16_t crc16;           /* CRC16/MODBUS 校验, 覆盖前面所有字节 (不含本字段) */
} NodeData_t;
#pragma pack(pop)

/* 节点证书(v2: 32 字节, 加 seq + crc16)
 * 首次上线时发送给网关, 网关代为上线 OneNET */
#pragma pack(push, 1)
typedef struct {
    uint8_t  valid;           /* 0=未配置, 1=有效 */
    char     ProductKey[12];  /* OneNET 产品ID */
    char     DeviceName[8];  /* OneNET 设备名称(如 Park001, 7字符+null) */
    char     FwVersion[8];  /* 节点固件版本(如 "v2.521", 6字符+null), 供网关 OTA 检测 */
    /* === v2 协议新增字段 === */
    uint8_t  seq;             /* 帧序列号 */
    uint16_t crc16;           /* CRC16/MODBUS 校验, 覆盖前面所有字节 */
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
