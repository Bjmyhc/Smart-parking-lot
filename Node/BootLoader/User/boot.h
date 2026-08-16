/****************************************************************************
 * BootLoader 接口头文件 - boot.h
 *
 * 功能: 函数声明、外部变量
 ****************************************************************************/

#ifndef __BOOT_H
#define __BOOT_H

#include <stdint.h>
#include "boot_cfg.h"

/* ==================== 外部变量 ==================== */
extern volatile uint32_t g_bootTick;    /* 系统滴答(ms) */

/* ==================== 函数声明 ==================== */

/* 跳转到 APP */
void Load_APP(uint32_t appxaddr);

/* 读/写升级标志页 */
uint32_t OTA_ReadFlag(void);
void OTA_WriteFlag(uint32_t flag);

/* 擦除 APP 区 */
uint8_t OTA_EraseAppArea(void);

/* Flash 写入 1 字节(实际按半字编程) */
uint8_t OTA_FlashWrite(uint32_t addr, const uint8_t *data, uint32_t len);

/* CRC16-XMODEM 计算 */
uint16_t CRC16_XMODEM(const uint8_t *data, uint32_t len);

/* 软件 CRC32 */
uint32_t CRC32_Soft(const uint8_t *data, uint32_t len);

/* 初始化(时钟/串口/看门狗/按键) */
void Boot_Init(void);

/* 初始化 LoRa 模块(USART2) */
void LoRa_Boot_Init(void);

/* 发送 1 字节到 LoRa 模块 */
void LoRa_Boot_SendByte(uint8_t b);

/* 从 LoRa 模块读取字节(非阻塞) */
uint8_t LoRa_Boot_ReadByte(uint8_t *b);

/* 喂狗 */
void Boot_FeedWatchdog(void);

/* 毫秒延时 */
void Boot_Delay(uint32_t ms);

/* 获取滴答 */
uint32_t Boot_GetTick(void);

#endif /* __BOOT_H */
