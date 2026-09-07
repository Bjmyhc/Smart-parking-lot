/****************************************************************************
 * 节点配置模块 - node_config.h
 *
 * 功能描述:
 *   读取 Flash 独立配置区中的节点身份信息(OneNET设备名/产品ID),
 *   填充到运行时变量供业务层统一引用.
 *   - 配置区(0x08003000)由 **Boot 首次启动**写入固化(见 BootLoader/User/boot_cfg.h),
 *     每个节点编译 Boot 时按需修改 NODE_DEVICE_NAME
 *   - OTA 只擦写 APP 区(0x08004000 起), 永不触碰本配置区 → OTA 后身份保留
 *   - 本模块**只读**配置区, 不携带任何身份默认值 → OTA 固件包纯净
 *
 * 部署流程:
 *   1. 每节点首次编译 Boot 时修改 boot_cfg.h 的设备名宏, ST-Link 烧录本节点
 *   2. Boot 首次上电自动固化身份到配置区
 *   3. 之后所有节点共用同一 App 固件/同一 OTA 包, 身份从配置区读取
 *
 * 运行时变量: g_nodeProductKey/g_nodeDeviceName,
 *   由 Config_Init() 启动时从配置区填充; 配置区无效时保持空值并报错.
 * 空中寻址使用 LoRa 模块硬件地址(ADDH/ADDL, 烧录时人工配置), 与代码无关,
 * 配置区不再存节点地址.
 *
 * Flash 选址: 0x08003000 (Boot 区 16KB 分配内的闲置页, Boot 实际占用仅 ~8.2KB)
 *
 * 作者: Bjmyhc
 * 日期: 2026-09-08
 ****************************************************************************/

#ifndef __NODE_CONFIG_H
#define __NODE_CONFIG_H

#include <stdint.h>

/* ==================== 运行时身份变量 ====================
 * 由 Config_Init() 在启动时从 Flash 配置区填充.
 * 所有使用身份信息的地方(证书上报等)一律引用这些变量. */
extern char g_nodeProductKey[12]; /* 实际生效的 OneNET 产品ID */
extern char g_nodeDeviceName[8];  /* 实际生效的 OneNET 子设备名 */

/* ==================== 配置扇区定义 ====================
 * 与 Boot 端 boot_cfg.h 保持一致 */
#define NODE_CONFIG_ADDR    0x08003000  /* 配置区起始地址(Boot分配区内闲置页) */
#define NODE_CONFIG_MAGIC   0xA5C3A5C3  /* 有效性标志 */

/* 节点身份配置结构体 (与 Boot 端 boot_cfg.h 完全一致) */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* 有效性标志 NODE_CONFIG_MAGIC */
    char     deviceName[8];  /* OneNET 子设备名 (如 Park001) */
    char     productKey[12]; /* OneNET 产品ID */
    uint32_t crc32;          /* 标准CRC32, 覆盖 magic..productKey 全部字节 */
} NodeConfig_t;
#pragma pack(pop)

/* ==================== 接口函数 ==================== */

/****************************************************************************
 * 初始化节点配置(必须在 LoRa_Node_Init 之前调用)
 * - 配置区有效: 读取并同步到运行时变量
 * - 配置区无效: 保持空值并打印错误(身份由 Boot 写入, 本模块不落盘)
 ****************************************************************************/
void Config_Init(void);

/****************************************************************************
 * 从 Flash 读取配置
 * 返回值: 1=成功(magic+CRC32校验通过), 0=无效/未配置
 ****************************************************************************/
uint8_t Config_Load(NodeConfig_t *cfg);

#endif /* __NODE_CONFIG_H */
