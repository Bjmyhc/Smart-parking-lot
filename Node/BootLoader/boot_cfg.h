/****************************************************************************
 * BootLoader 配置头文件 - boot_cfg.h
 *
 * 功能: 分区地址、协议参数、超时等所有可配置选项
 *
 * 硬件: STM32F103C8T6, 64KB Flash, 20KB RAM
 ****************************************************************************/

#ifndef __BOOT_CFG_H
#define __BOOT_CFG_H

/* ==================== Flash 分区 ==================== */
/* 单区方案: Boot 8KB + APP 55KB + 标志页 1KB
 * BootLoader 常驻 0x08000000, 永不覆盖
 * OTA 时直接覆盖 APP 区, 断电会损坏 APP 但 Boot 永远能救回 */

/* APP 起始地址(0x08002000, 页8) */
#define APP_ADDR                0x08002000

/* APP 最大长度(字节), 55KB */
#define APP_MAX_SIZE            (55 * 1024)

/* 升级标志页地址(0x0800FC00, 页252, 最后1KB) */
#define OTA_FLAG_ADDR           0x0800FC00

/* 标志值 */
#define OTA_FLAG_GO             0xA5A5A5A5  /* 需要升级 */
#define OTA_FLAG_DONE           0x00000000  /* 升级完成 */

/* ==================== 固件文件头(12字节) ==================== */
/* 与 Tool/fw_pack.py 一致 */
#define FW_MAGIC                0xA55A

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;      /* 魔数 0xA55A */
    uint16_t version;    /* 固件版本 */
    uint32_t length;     /* 代码段长度 */
    uint32_t crc32;      /* 代码段 CRC32 */
} FwHeader_t;
#pragma pack(pop)

/* ==================== 升级协议 ==================== */
/* Xmodem 风格, 链路用 LoRa 定点传输 */

/* 控制字符 */
#define SOH 0x02    /* 数据包开始 */
#define ACK 0x03    /* 确认 */
#define NAK 0x15    /* 否定 */
#define EOT 0x04    /* 传输结束 */
#define CAN 0x18    /* 取消 */

/* 数据包大小 */
#define OTA_PACKET_DATA_SIZE    128

/* 超时(毫秒) */
#define OTA_PACKET_TIMEOUT_MS   1000    /* 每包等待 */
#define OTA_TOTAL_TIMEOUT_MS    120000  /* 总超时(2分钟) */

/* 每包最大重试次数 */
#define OTA_MAX_RETRY           3

/* ==================== 硬件配置 ==================== */
#define LORA_BAUD               9600
#define WDG_TIMEOUT_MS          4000    /* 看门狗超时, 与 APP 一致 */

/* 上电窗口(毫秒), 按键/OTA 指令进入 Boot 的等待时间 */
#define BOOT_ENTER_WINDOW_MS    2000

#endif /* __BOOT_CFG_H */