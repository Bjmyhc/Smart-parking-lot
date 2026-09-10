/****************************************************************************
 * BootLoader 配置头文件 - boot_cfg.h
 *
 * 功能: 分区地址、协议参数、超时等所有可配置选项
 *
 * 硬件: STM32F103C8T6, 64KB Flash, 20KB RAM
 ****************************************************************************/

#ifndef __BOOT_CFG_H
#define __BOOT_CFG_H

/* ⭐ S11 协议单一事实源: 控制字符(SOH/ACK/NAK/EOT/CAN)/OTA_FW_MAGIC/
 * OTA_PACKET_DATA_SIZE/LORA_FRAME_ACK/LORA_GATEWAY_ADDR/LORA_CHANNEL
 * 统一由共享头 lora_protocol.h 提供 (shared/ 目录, 与网关/App 强制一致) */
#include "lora_protocol.h"

/* ==================== 节点身份配置区 ====================
 * ⭐ 节点身份由 Boot 首次启动写入配置区(0x08003000), OTA 只擦写 APP 区,
 *    配置区不受影响 → 各节点可共用同一份纯净的 OTA 固件包.
 *
 * 部署流程:
 *   1. 每个节点首次编译 Boot 时按需修改 NODE_DEVICE_NAME
 *      (节点1=Park001, 节点2=Park002..., 设备名是节点唯一标识)
 *   2. ST-Link 烧录本节点 Boot, 首次上电自动固化到 Flash 配置区
 *   3. 之后所有节点共用同一 App 固件/同一 OTA 包, 身份从配置区读取
 *
 * 注意: App 端(node_config.h)已删除这些默认宏, 只从配置区读取身份,
 *       以保证 OTA 固件不携带任何节点身份信息.
 * 空中寻址使用 LoRa 模块硬件地址(ADDH/ADDL, 烧录时人工配置), 与代码无关,
 * 本配置区不再存节点地址, 仅存 OneNET 证书(产品ID/设备名). */
#define NODE_CONFIG_ADDR    0x08003000  /* 配置区地址(Boot分配区内闲置页) */
#define NODE_CONFIG_MAGIC   0xA5C3A5C3  /* 配置区有效性标志 */

#ifndef NODE_PRODUCT_KEY
#define NODE_PRODUCT_KEY    "04kjwU9TC7"    /* 子设备产品ID */
#endif
#ifndef NODE_DEVICE_NAME
#define NODE_DEVICE_NAME    "Park002"       /* 子设备设备名(每节点唯一) */
#endif

/* 节点身份配置结构体(与 App 端 node_config.h 完全一致, 按2字节对齐写Flash) */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          /* 有效性标志 NODE_CONFIG_MAGIC */
    char     deviceName[8];  /* OneNET 子设备名 (如 Park001) */
    char     productKey[12]; /* OneNET 产品ID */
    uint32_t crc32;          /* 标准CRC32, 覆盖 magic..productKey 全部字节 */
} NodeConfig_t;
#pragma pack(pop)

/* ==================== Flash 分区 ==================== */
/* 单区方案(64KB Flash):
 *   Boot 0x08000000 (16KB 分配, 实际占用约 8.2KB)
 *   配置区 0x08003000 (1页, 节点身份, OTA 不受影响)
 *   APP 0x08004000 (最大 47KB, 0x08004000~0x0800FBFF)
 *   升级标志页 0x0800FC00 (最后1KB, 与 APP 区互不重叠)
 * BootLoader 常驻 0x08000000, 永不覆盖
 * OTA 时直接覆盖 APP 区, 断电会损坏 APP 但 Boot 永远能救回 */

/* APP 起始地址(页16) */
#define APP_ADDR                0x08004000

/* APP 最大长度(字节), 47KB = 0xBC00.
 * 约束: 0x08004000 + 47KB = 0x0800FC00 恰好到标志页起始(最后1KB),
 *       固件最大占用 0x08004000~0x0800FBFF, 不覆盖 0x0800FC00 升级标志页.
 * 切勿超过 48KB(0xC000), 否则 OTA 可能写穿 Flash 或覆盖标志页. */
#define APP_MAX_SIZE            (47 * 1024)

/* ⭐ OTA 升级标志页常量与读写接口已迁移到共享头 boot_flash.h (S8/S10 单一事实源)
 *    Boot/App 统一引用 <boot_flash.h>, 此处不再重复定义 */

/* ==================== 固件文件头(12字节) ==================== */
/* 与 Tool/fw_pack.py 一致; 魔数 OTA_FW_MAGIC 已由共享头 lora_protocol.h 提供 (S11) */

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;      /* 魔数 0xA55A */
    uint16_t version;    /* 固件版本 */
    uint32_t length;     /* 代码段长度 */
    uint32_t crc32;      /* 代码段 CRC32 */
} FwHeader_t;
#pragma pack(pop)

/* ==================== 升级协议 ==================== */
/* Xmodem 风格, 链路用 LoRa 定点传输
 * 控制字符(SOH/ACK/NAK/EOT/CAN) 与 数据包大小 OTA_PACKET_DATA_SIZE
 * 已由共享头 lora_protocol.h 提供 (S11), 此处不再重复定义 */

/* 超时(毫秒) */
#define OTA_PACKET_TIMEOUT_MS   1000    /* 每包等待 */
#define OTA_TOTAL_TIMEOUT_MS    180000  /* 总超时(3分钟), 擦除APP后计时, 与网关对齐 */

/* ⭐ 擦除后催发间隔(毫秒): APP 区已擦除且长时间收不到数据包时,
 * 每间隔该时间主动发一次 NAK 催促网关重发(整链重试闭环的一部分),
 * 配合 180s 总超时, 等待网关重新触发/重发 */
#define OTA_ERASED_NAK_INTERVAL_MS 10000

/* ⭐ 擦除后NAK催发上限次数: 网关连续无响应时停止催发,
 * 防止网关侧任务异常(如文件被注销)后节点无限刷屏"催促网关" */
#define OTA_ERASED_NAK_MAX       5

/* ⭐ 首包等待(毫秒): 两阶段提交下, 收到有效固件头之前 APP 区未被擦除,
 * 旧 APP 完好 → 首包超时即可清标志回退旧 APP, 保证 ACK 丢失等
 * "网关放弃"场景节点 ≤30s 自动恢复, 不进入"节点死亡"状态 */
#define OTA_FIRST_PKT_TIMEOUT_MS 15000

/* 每包最大重试次数 */
#define OTA_MAX_RETRY           3

/* ==================== LoRa 定点地址 ==================== */
/* 定点传输发送格式: [AddrH][AddrL][CH] + 数据
 * Boot 回复 ACK/NAK 等必须带目标(网关)地址头, 否则 LoRa 模块
 * 会把首个字节当地址头解析, 响应发不出去
 * LORA_GATEWAY_ADDR / LORA_CHANNEL 已由共享头 lora_protocol.h 提供 (S11) */

/* ==================== 硬件配置 ==================== */
#define LORA_BAUD               9600
#define WDG_TIMEOUT_MS          4000    /* 看门狗超时, 与 APP 一致 */

/* 上电窗口(毫秒), 按键/OTA 指令进入 Boot 的等待时间 */
#define BOOT_ENTER_WINDOW_MS    2000

#endif /* __BOOT_CFG_H */
