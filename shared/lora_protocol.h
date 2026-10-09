/* lora_protocol.h - LoRa 协议单一事实源 (S11)
 *
 * ⭐ 本文件是协议常量/数据结构/校验函数的唯一权威定义 (单一事实源):
 *   - 网关端: Gateway/lora_protocol.h 只做薄包装 include 本文件 + 网关调度参数
 *   - 节点端: Node/Firmware/BSP/bsp_lora.h 与 Node/BootLoader/User/boot_cfg.h
 *             include 本文件, 不再各自重复定义帧头/结构体/控制字符
 *   - 任何协议改动只改本文件, 双端构建期 include 即强制一致
 *
 * 只包含协议本身(帧头/定点地址/数据结构/命令格式/Xmodem 控制字符),
 * 不包含任何单端调度参数(网关调度参数在 Gateway/lora_protocol.h,
 * 节点超时参数在 boot_cfg.h)。
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-08 (v3)
 * 2026-09-09 S11: 收敛为双端共享单一事实源
 * 2026-10-09 (v4): 车牌子系统接入 —— 数据帧加 CamFlags(18→19B), 新增车牌事件帧 LoraPlate_t(0xF1)
 */
#ifndef __SHARED_LORA_PROTOCOL_H
#define __SHARED_LORA_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>   /* offsetof (CRC 计算用) */

/* ============ 协议版本 ============ */
#define LORA_PROTO_VERSION    4   /* v4: 数据帧新增 CamFlags 字节(18→19B) + 新增车牌事件帧 0xF1
                                   * ⚠ 本版为结构性变更: 节点与网关必须用串口线**同时烧**,
                                   *   禁止走 OTA(OTA 必然一先一后, 中间窗口长度不符 → 数据帧全丢) */

/* ============ CRC16/MODBUS (工业标准, 多项式 0xA001) ============
 * 覆盖范围: 整个结构体除 crc16 字段外的所有字节
 * 漏检概率 ~ 1/65536, 对 19~34 字节短帧完全够用 (LoRaWAN 也用 CRC16)
 * 两端共用此函数, static inline 避免 link 冲突 */
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

/* ============ 定点传输配置 ============ */
#define LORA_GATEWAY_ADDR       0x0000   /* 网关固定地址 */
#define LORA_CHANNEL            0x00     /* 信道(0), DX-LR22模块: 00=433.15MHz */

/* 定点传输帧头(3字节): [AddrH][AddrL][CH]
 * 发送前要加在数据前面 */
#define LORA_FRAME_HEADER_SIZE  3

/* ============ 帧头字节 (子设备→网关) ============ */
#define LORA_FRAME_CERT         0xA1    /* 证书数据(响应 AT+CERx) */
#define LORA_FRAME_DATA         0xB1    /* 传感器数据(响应 AT+DATAx) */
#define LORA_FRAME_ACK          0xC1    /* 命令执行确认(响应 AT+SetLed 等) */
#define LORA_FRAME_OTA_OK       0xD1    /* OTA接收128B成功, 准备下一包 */
#define LORA_FRAME_OTA_RETRY    0xE1    /* OTA要求重发上一包 */
#define LORA_FRAME_PLATE        0xF1    /* ⭐ v4: 车牌识别结果(响应 AT+PLATE, 事件量走独立帧) */

/* ============ 节点传感器数据 ============
 * 字段顺序、类型、对齐双端强制一致 (本文件为唯一来源)。
 * 布局用 #pragma pack(1): Keil armcc / AC6 / GCC(ESP8266) 均支持 */

/* 节点传感器数据帧(v4: 19 字节, 加 CamFlags + seq + crc16)
 * LORA_FRAME_DATA + 下面结构体
 * ⭐ v4 协议: 在 seq 之前插入 CamFlags(1B), 18B → 19B (车牌子系统状态量)
 * ⭐ v3 协议: 移除 LedEnable 字段(平台原 LedEnable 属性已迁移为 SetLed 服务), 19B → 18B
 * ⭐ v2 协议: 末尾追加 seq(1B) + crc16(2B), 16B → 19B
 *   - seq: 节点每次发送 ++, 0..255 循环 (网关端可记录检测重复/丢包)
 *   - crc16: CRC16/MODBUS, 覆盖 [结构体首, offsetof(crc16)) 字节
 * 节点端发送前填, 网关端接收校验, 不通过直接丢弃 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  ParkStatus;      /* 0=空闲, 1=有车, 2=僵尸车 */
    uint8_t  GeoMagnetic;     /* 0/1 地磁检测值 */
    uint16_t Ultrasonic;      /* cm, 小端 */
    uint32_t OccupiedTime;    /* 秒, 小端 */
    uint8_t  LED;             /* LED(报警灯)当前状态 0/1 */
    uint32_t ZombieThreshold; /* ⭐ 僵尸车判定阈值(秒), 节点当前生效值, 网关据此上报只读属性观看 */
    uint16_t SensorDistanceCm; /* ⭐ 超声波判定距离阈值(cm), 节点当前生效值 */
    /* === v4 协议新增字段: 车牌子系统状态标志位 === */
    uint8_t  CamFlags;        /* bit0~1 = 拍照策略(0~3)
                               * bit2   = 有新车牌待取(网关据此发 AT+PLATE)
                               * bit3   = 摄像头在线(节点探活结果, 1=在线)
                               * bit4~7 = 预留(以后再要加状态位无需改协议) */
    /* === v2 协议新增字段 (放末尾, 兼容前向布局) === */
    uint8_t  seq;             /* 帧序列号, 节点每次发送 ++, 0..255 循环 */
    uint16_t crc16;           /* CRC16/MODBUS 校验, 覆盖前面所有字节 (不含本字段) */
} LoraNodeData_t;

/* 车牌事件帧(v4: 34 字节)
 * LORA_FRAME_PLATE + 下面结构体。车牌为**事件量**, 独立成帧, 不塞进数据帧。
 * ⚠ 本结构体**没有 seq 字段** —— 判重启只能靠 frameNo 回退, 且摄像头重启后
 *   frameNo 会归零, 不能单靠它判新(网关侧需结合差异检测) */
typedef struct {
    char     plate[24];   /* UTF-8: 中文 3 字节/字, 24B 够 8 字符车牌(含中点) + null */
    uint8_t  conf;        /* 置信度 0~100 */
    uint8_t  valid;       /* 0=未识别($PLATE,-), 1=有效 */
    uint8_t  source;      /* 1=节点自动 2=软件手动 0=未知 (仅排障, 不上云) */
    uint32_t frameNo;     /* 摄像头帧号($PLATE 第 3 段, 仅排障, 不上云) */
    uint8_t  color;       /* 预留: 车牌颜色 0=未知 1=蓝 2=黄 3=绿 4=白 5=黑 (一期恒为 0, 不上报) */
    uint16_t crc16;       /* CRC16/MODBUS, 覆盖 [结构体首, offsetof(crc16)) = 前 32 字节 */
} LoraPlate_t;            /* 34 字节 = 24+1+1+1+4+1+2 */

/* 节点证书帧(v2: 32 字节, 加 seq + crc16)
 * LORA_FRAME_CERT + 下面结构体 */
typedef struct {
    uint8_t  valid;                    /* 0=未配置, 1=有效 */
    char     ProductKey[12];           /* OneNET 产品ID */
    char     DeviceName[8];            /* OneNET 设备名称(如 Park001, 7字符+null) */
    char     FwVersion[8];            /* 节点固件版本(如 "v2.521", 6字符+null), 供网关 OTA 检测 */
    /* === v2 协议新增字段 === */
    uint8_t  seq;                      /* 帧序列号 */
    uint16_t crc16;                    /* CRC16/MODBUS 校验, 覆盖前面所有字节 */
} LoraNodeCert_t;
#pragma pack(pop)

/* ============ 下行命令格式 ============
 * 网关使用定点传输(目标节点地址) 发送 ASCII 命令:
 *   AT+CER\r\n            查询节点证书
 *   AT+DATA\r\n           查询节点数据
 *   AT+PING\r\n           探测节点是否在线
 *   AT+SetLed=<v>\r\n  设置节点报警灯使能(僵尸车报警灯), v=0/1
 *   AT+CAPTURE\r\n        手动触发拍照识别(无参), 节点回 ACK 后置"有牌待取"旗子
 *   AT+PLATE\r\n          取车牌(无参), 节点回一帧 LORA_FRAME_PLATE(0xF1) 并清旗子
 *   AT+CapturePolicy=<0..3>\r\n  设置拍照策略(0不拍/1有车/2僵尸车/3都拍)
 *   AT+OTA=start,V<m>.<n>\r\n  触发节点OTA升级
 *
 * ⚠ 命令名不得包含既有命令名(大小写敏感)作为子串; AT+CAPTURE 与 AT+CapturePolicy
 *   一律不许改成全大写写法(靠大小写区分, 改动会破坏网关 ACK 归属匹配)
 *
 * 注意: 命令名不携带节点号, 节点身份由定点传输帧头[AddrH][AddrL]区分
 */

/* 命令最大长度(含\r\n) */
#define LORA_CMD_MAX_LEN       64

/* ============ OTA 升级协议 (Xmodem 风格) ============ */

/* Xmodem 控制字符 (节点端 BootLoader 与网关共用本定义) */
#define OTA_SOH                 0x02    /* 数据包开始 */
#define OTA_ACK                 0x03    /* 确认 */
#define OTA_NAK                 0x15    /* 否定 */
#define OTA_EOT                 0x04    /* 传输结束 */
#define OTA_CAN                 0x18    /* 取消 */

/* 数据包大小 */
#define OTA_PACKET_DATA_SIZE    128

/* 固件文件头魔数(与 Tool/fw_pack.py 一致, 与 BootLoader 端 boot_cfg.h 一致) */
#define OTA_FW_MAGIC            0xA55A

#ifdef __cplusplus
}
#endif

#endif /* __SHARED_LORA_PROTOCOL_H */
