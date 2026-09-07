/****************************************************************************
 * BootLoader 配置头文件 - boot_cfg.h
 *
 * 功能: 分区地址、协议参数、超时等所有可配置选项
 *
 * 硬件: STM32F103C8T6, 64KB Flash, 20KB RAM
 ****************************************************************************/

#ifndef __BOOT_CFG_H
#define __BOOT_CFG_H

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
#define NODE_DEVICE_NAME    "Park001"       /* 子设备设备名(每节点唯一) */
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

/* 升级标志页地址(0x0800FC00, 页63, 最后1KB) */
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
#define OTA_TOTAL_TIMEOUT_MS    120000  /* 总超时(2分钟), 擦除APP后计时 */

/* ⭐ 首包等待(毫秒): 两阶段提交下, 收到有效固件头之前 APP 区未被擦除,
 * 旧 APP 完好 → 首包超时即可清标志回退旧 APP, 保证 ACK 丢失等
 * "网关放弃"场景节点 ≤30s 自动恢复, 不进入"节点死亡"状态 */
#define OTA_FIRST_PKT_TIMEOUT_MS 15000

/* 每包最大重试次数 */
#define OTA_MAX_RETRY           3

/* ==================== LoRa 定点地址 ==================== */
/* 定点传输发送格式: [AddrH][AddrL][CH] + 数据
 * Boot 回复 ACK/NAK 等必须带目标(网关)地址头, 否则 LoRa 模块
 * 会把首个字节当地址头解析, 响应发不出去 */
#define LORA_GATEWAY_ADDR       0x0000  /* 网关地址 */
#define LORA_CHANNEL            0x00    /* 信道(0), DX-LR22模块: 00=433.15MHz */

/* ==================== 硬件配置 ==================== */
#define LORA_BAUD               9600
#define WDG_TIMEOUT_MS          4000    /* 看门狗超时, 与 APP 一致 */

/* 上电窗口(毫秒), 按键/OTA 指令进入 Boot 的等待时间 */
#define BOOT_ENTER_WINDOW_MS    2000

#endif /* __BOOT_CFG_H */
