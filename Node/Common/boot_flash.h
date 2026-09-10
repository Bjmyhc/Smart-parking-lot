/****************************************************************************
 * Boot/App 共用 Flash 标志页写接口 - boot_flash.h
 *
 * 功能: OTA 升级标志页(0x0800FC00)常量 + 统一读写接口 (单一事实源, S8/S10)
 *       双槽位语义:
 *         槽位1 @OTA_FLAG_ADDR     升级标志 GO/DONE
 *         槽位2 @OTA_FLAG_APP_ADDR APP 区完整性标志 CLEAN/PARTIAL
 *       所有写入统一走 Flash_SaveOtaFlag(): 擦一次页, 同时写两个槽位,
 *       保证断电时刻两端对标志页状态的理解一致 (App 不再裸写只写 GO 槽位).
 *
 * 硬件: STM32F103C8T6. Boot 与 App 两个 Keil 工程均启用 C99,
 *       故用 static inline 实现, 无需新增 .c 文件.
 ****************************************************************************/

#ifndef __BOOT_FLASH_H
#define __BOOT_FLASH_H

#include "stm32f10x_flash.h"

/* ==================== OTA 升级标志页 (单一事实源) ==================== */
#define OTA_FLAG_ADDR           0x0800FC00  /* 标志页地址(页63, 最后1KB) */
#define OTA_FLAG_GO             0xA5A5A5A5  /* 需要升级 */
#define OTA_FLAG_DONE           0x00000000  /* 升级完成 */
#define OTA_FLAG_APP_CLEAN      0xFFFFFFFF  /* APP 区完好(出厂擦除态) */
#define OTA_FLAG_APP_PARTIAL    0x5A5A5A5A  /* APP 区已擦除/写入不完整 */
#define OTA_FLAG_APP_ADDR       (OTA_FLAG_ADDR + 4)  /* APP 完整性标志地址 */

/* 读升级标志(槽位1) */
static inline uint32_t Flash_ReadOtaFlag(void)
{
    return *(volatile uint32_t *)OTA_FLAG_ADDR;
}

/* 读 APP 完整性标志(槽位2, 断电可持久) */
static inline uint32_t Flash_ReadAppPartial(void)
{
    return *(volatile uint32_t *)OTA_FLAG_APP_ADDR;
}

/* 同时写升级标志 + APP 完整性标志 (同一标志页, 擦一次写两个槽位) */
static inline void Flash_SaveOtaFlag(uint32_t flag, uint32_t appPartial)
{
    FLASH_Unlock();
    FLASH_ErasePage(OTA_FLAG_ADDR);
    FLASH_ProgramHalfWord(OTA_FLAG_ADDR, (uint16_t)(flag & 0xFFFF));
    FLASH_ProgramHalfWord(OTA_FLAG_ADDR + 2, (uint16_t)((flag >> 16) & 0xFFFF));
    FLASH_ProgramHalfWord(OTA_FLAG_APP_ADDR, (uint16_t)(appPartial & 0xFFFF));
    FLASH_ProgramHalfWord(OTA_FLAG_APP_ADDR + 2, (uint16_t)((appPartial >> 16) & 0xFFFF));
    FLASH_Lock();
}

/* 写升级标志, APP 完整性标志保持不变 (Boot 触发/App 触发通用) */
static inline void Flash_SaveOtaFlagKeepPartial(uint32_t flag)
{
    Flash_SaveOtaFlag(flag, Flash_ReadAppPartial());
}

/* 写 APP 完整性标志, 升级标志保持不变 */
static inline void Flash_SetAppPartial(uint8_t partial)
{
    Flash_SaveOtaFlag(Flash_ReadOtaFlag(),
                      partial ? OTA_FLAG_APP_PARTIAL : OTA_FLAG_APP_CLEAN);
}

#endif /* __BOOT_FLASH_H */
