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
#include <stddef.h>   /* offsetof (CRC 计算用) */

/* ============ 协议版本 ============ */
#define LORA_PROTO_VERSION    2   /* v2: 加 seq + crc16 字段 */

/* ============ CRC16/MODBUS (工业标准, 多项式 0xA001) ============
 * 覆盖范围: 整个结构体除 crc16 字段外的所有字节
 * 漏检概率 ~ 1/65536, 对 19-32 字节短帧完全够用 (LoRaWAN 也用 CRC16)
 * 两端共用此函数, static inline 避免 link 冲突 */
static inline uint16_t lora_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else         crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

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

/* 节点传感器数据帧(v2: 19 字节, 加 seq + crc16)
 * LORA_FRAME_DATA + 下面结构体
 * ⭐ v2 协议: 末尾追加 seq(1B) + crc16(2B), 16B → 19B
 *   - seq: 节点每次发送 ++, 0..255 循环 (网关端可记录检测重复/丢包)
 *   - crc16: CRC16/MODBUS, 覆盖 [结构体首, offsetof(crc16)) 字节
 * 节点端发送前填, 网关端接收校验, 不通过直接丢弃 */
typedef struct LORA_PACKED {
    uint8_t  ParkStatus;      /* 0=空闲, 1=有车, 2=僵尸车 */
    uint8_t  GeoMagnetic;     /* 0/1 地磁检测值 */
    uint16_t Ultrasonic;      /* cm, 小端 */
    uint32_t OccupiedTime;    /* 秒, 小端 */
    uint8_t  LED;             /* 当前LED状态 0/1 */
    uint8_t  LedEnable;       /* LED使能开关 0/1 */
    uint32_t ZombieThreshold; /* ⭐ 僵尸车判定阈值(秒), 节点当前生效值, 网关据此上报只读属性观看 */
    uint16_t SensorDistanceCm; /* ⭐ 超声波判定距离阈值(cm), 节点当前生效值 */
    /* === v2 协议新增字段 (放末尾, 兼容前向布局) === */
    uint8_t  seq;             /* 帧序列号, 节点每次发送 ++, 0..255 循环 */
    uint16_t crc16;           /* CRC16/MODBUS 校验, 覆盖前面所有字节 (不含本字段) */
} LoraNodeData_t;

/* 节点证书帧(v2: 32 字节, 加 seq + crc16)
 * LORA_FRAME_CERT + 下面结构体 */
typedef struct LORA_PACKED {
    uint8_t  valid;                    /* 0=未配置, 1=有效 */
    char     ProductKey[12];           /* OneNET 产品ID */
    char     DeviceName[8];            /* OneNET 设备名称(如 Park001, 7字符+null) */
    char     FwVersion[8];            /* 节点固件版本(如 "v2.521", 6字符+null), 供网关 OTA 检测 */
    /* === v2 协议新增字段 === */
    uint8_t  seq;                      /* 帧序列号 */
    uint16_t crc16;                    /* CRC16/MODBUS 校验, 覆盖前面所有字节 */
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

/* 节点复位等待时间(ms): 发送 AT+OTA 收到节点 ACK 后, 节点复位进 BootLoader 需要时间 */
#define OTA_NODE_RESET_WAIT_MS  5000

/* ⭐ AT+OTA 触发命令可靠投递: 发命令后等节点 ACK, 丢包则重发 */
#define OTA_TRIGGER_ACK_TIMEOUT_MS  2000   /* 每次发送后等节点 ACK 的超时(ms) */
#define OTA_TRIGGER_MAX_SEND         3     /* 最多发送次数(含首次, 防 LoRa 丢包) */

/* 固件文件头(12字节, 与 Tool/fw_pack.py 一致, 与 BootLoader 端 boot_cfg.h 一致) */
#define OTA_FW_MAGIC            0xA55A

/* OTA 固件临时文件路径 (LittleFS) */
#define OTA_FW_FILE             "/ota_firmware.bin"

#ifdef __cplusplus
}
#endif

#endif /* LORA_PROTOCOL_H */
