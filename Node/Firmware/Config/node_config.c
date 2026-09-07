/****************************************************************************
 * 节点配置模块 - node_config.c
 *
 * 功能描述:
 *   从 Flash 独立配置区读取节点身份(设备名/产品ID), 同步到运行时变量.
 *   配置区由 **Boot 首次启动**写入固化(见 BootLoader/User/main.c 的 NodeConfig_Init),
 *   本模块只读, 不携带任何身份默认值 → OTA 固件包纯净.
 *
 * 运行时身份来源:
 *   配置区(0x08003000)有效 → 读取并填充运行时变量
 *   配置区无效            → 保持空值并报错, 节点无法入网
 * 空中寻址使用 LoRa 模块硬件地址(ADDH/ADDL, 烧录时人工配置), 与代码无关.
 *
 * 作者: Bjmyhc
 * 日期: 2026-09-08
 ****************************************************************************/

#include <string.h>
#include <stddef.h>   /* offsetof */
#include "node_config.h"
#include "bsp_usart.h"

/* ==================== 运行时身份变量 ==================== */

/* 实际生效的节点身份(启动时由 Config_Init 从 Flash 配置区填充, 初始为空) */
char g_nodeProductKey[12];
char g_nodeDeviceName[8];

/* ==================== 内部函数 ==================== */

/* 标准 CRC32 (zlib 参数: 多项式 0xEDB88320, 初值/异或输出 0xFFFFFFFF) */
static uint32_t CRC32_Soft(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i, b;
    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ==================== 接口函数 ==================== */

uint8_t Config_Load(NodeConfig_t *cfg)
{
    const NodeConfig_t *src = (const NodeConfig_t *)NODE_CONFIG_ADDR;
    const uint8_t *p = (const uint8_t *)src;

    /* 未编程 Flash 为 0xFF, magic 必不等于 NODE_CONFIG_MAGIC → 视为未配置 */
    if (src->magic != NODE_CONFIG_MAGIC)
        return 0;
    if (CRC32_Soft(p, offsetof(NodeConfig_t, crc32)) != src->crc32)
        return 0;
    *cfg = *src;
    return 1;
}

void Config_Init(void)
{
    NodeConfig_t cfg;

    if (Config_Load(&cfg))
    {
        /* 配置区有效: 同步到运行时变量 */
        memset(g_nodeProductKey, 0, sizeof(g_nodeProductKey));
        memset(g_nodeDeviceName, 0, sizeof(g_nodeDeviceName));
        strncpy(g_nodeProductKey, cfg.productKey, sizeof(g_nodeProductKey) - 1);
        strncpy(g_nodeDeviceName, cfg.deviceName, sizeof(g_nodeDeviceName) - 1);
        Usart_Printf(USART_DEBUG, "[CFG] 读取节点配置: 产品=%s 设备=%s\r\n",
                     g_nodeProductKey, g_nodeDeviceName);
        return;
    }

    /* 配置区无效: 保持空值, 节点无法入网.
     * 身份由 Boot 首次启动写入, 此处不落盘, 保证 OTA 固件纯净.
     * 若出现此错误, 说明该节点 Boot 未正确写入配置区, 需重新烧录 Boot. */
    memset(g_nodeProductKey, 0, sizeof(g_nodeProductKey));
    memset(g_nodeDeviceName, 0, sizeof(g_nodeDeviceName));
    Usart_Printf(USART_DEBUG, "[CFG][ERR] 配置区无效, 节点无身份! 请重新烧录Boot\r\n");
}
