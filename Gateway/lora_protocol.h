/* lora_protocol.h - LoRa 协议统一常量/结构体
 *
 * 说明: 本文件只包含协议本身(帧头/定点地址/数据结构/命令格式),
 *       不包含任何网关调度参数(那些在 app_cfg.h)。
 *       网关端 STM32 节点端(BSP/lora_node.h) 各自维护一份副本,
 *       任何结构体修改需两端人工同步(字段顺序/类型/对齐必须一致)。
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-08
 */
#ifndef LORA_PROTOCOL_H
#define LORA_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ============ 定点传输配置 ============ */
#define LORA_GATEWAY_ADDR       0x0000   /* 网关固定地址 */
#define LORA_CHANNEL            0x00     /* 信道(0), DX-LR22模块: 00=433.15MHz */

/* 定点传输帧头(3字节): [AddrH][AddrL][CH]
 * 发送前要加在数据前面 */
#define LORA_FRAME_HEADER_SIZE  3

/* ============ 帧头字节 (子设备→网关) ============ */
#define LORA_FRAME_CERT         0xA1    /* 证书数据(响应 AT+CERx) */
#define LORA_FRAME_DATA         0xB1    /* 传感器数据(响应 AT+DATAx) */
#define LORA_FRAME_ACK          0xC1    /* 命令执行确认(响应 AT+LedEnable 等) */
#define LORA_FRAME_OTA_OK       0xD1    /* OTA接收128B成功, 准备下一包 */
#define LORA_FRAME_OTA_RETRY    0xE1    /* OTA要求重发上一包 */

/* ============ 节点传感器数据 ============
 * 注意: 字段顺序、类型、对齐必须与 STM32 端 lora_node.h 完全一致
 * 任何改动需同步修改两端 */

#if defined(__cplusplus) && defined(ARDUINO)
/* ESP8266 / ESP32 Arduino 使用 packed 属性 */
#define LORA_PACKED  __attribute__((packed))
#else
/* STM32 KEIL 使用 pragma pack(在 include 前外部打开) */
#define LORA_PACKED
#endif

/* 节点传感器数据帧(8 字段, 共 12 字节)
 * LORA_FRAME_DATA + 下面结构体 */
typedef struct LORA_PACKED {
    uint8_t  ParkStatus;      /* 0=空闲, 1=有车, 2=僵尸车 */
    uint8_t  GeoMagnetic;     /* 0/1 地磁检测值 */
    uint16_t Ultrasonic;      /* cm, 小端 */
    uint32_t OccupiedTime;    /* 秒, 小端 */
    uint8_t  LED;             /* 当前LED状态 0/1 */
    uint8_t  LedEnable;       /* LED使能开关 0/1 */
    char     FwVersion[16];   /* 节点固件版本字符串(如 "v2.321"), 与 STM32 端一致 */
} LoraNodeData_t;

/* 节点证书帧(供网关代上线 OneNET)
 * LORA_FRAME_CERT + 下面结构体 */
typedef struct LORA_PACKED {
    uint8_t  valid;                    /* 0=未配置, 1=有效 */
    char     ProductKey[12];           /* OneNET 产品ID */
    char     DeviceName[33];           /* OneNET 设备名称(park1/park2...) */
    char     AccessKey[33];            /* OneNET 设备密钥 */
} LoraNodeCert_t;

/* ============ 下行命令格式 ============
 * 网关使用定点传输(目标节点地址) 发送 ASCII 命令:
 *   AT+CER\r\n            查询节点证书
 *   AT+DATA\r\n           查询节点数据
 *   AT+PING\r\n           探测节点是否在线
 *   AT+LedEnable=<v>\r\n  设置节点LedEnable, v=0/1
 *   AT+OTA=start,V<m>.<n>\r\n  触发节点OTA升级
 *
 * 注意: 命令名不携带节点号, 节点身份由定点传输帧头[AddrH][AddrL]区分
 */

/* 命令最大长度(含\r\n) */
#define LORA_CMD_MAX_LEN       64

/* ============ OTA 升级协议 (Xmodem 风格) ============ */

/* Xmodem 控制字符 (与节点端 BootLoader 一致) */
#define OTA_SOH                 0x02    /* 数据包开始 */
#define OTA_ACK                 0x03    /* 确认 */
#define OTA_NAK                 0x15    /* 否定 */
#define OTA_EOT                 0x04    /* 传输结束 */
#define OTA_CAN                 0x18    /* 取消 */

/* 数据包大小 */
#define OTA_PACKET_DATA_SIZE    128

/* 超时(毫秒) */
#define OTA_PACKET_TIMEOUT_MS   2000    /* 每包等待节点回复 */
#define OTA_TOTAL_TIMEOUT_MS    180000  /* 总超时(3分钟) */

/* 每包最大重试次数 */
#define OTA_MAX_RETRY           3

/* 节点复位等待时间(ms): 发送 AT+OTA 后, 节点复位进 BootLoader 需要时间 */
#define OTA_NODE_RESET_WAIT_MS  5000

/* 固件文件头(12字节, 与 Tool/fw_pack.py 一致, 与 BootLoader 端 boot_cfg.h 一致) */
#define OTA_FW_MAGIC            0xA55A

/* OTA 固件临时文件路径 (LittleFS) */
#define OTA_FW_FILE             "/ota_firmware.bin"

#ifdef __cplusplus
}
#endif

#endif /* LORA_PROTOCOL_H */
