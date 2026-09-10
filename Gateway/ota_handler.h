/* ota_handler.h - Gateway OTA 升级处理器
 *
 * 功能: 从网络下载固件 → 通过 LoRa 分包发送给节点
 *
 * 流程:
 *   ota_start(nodeId, url) 触发 OTA:
 *     1. 先通过 LoRa 发送 AT+OTA=start,V<m>.<n> 触发节点复位进 BootLoader
 *     2. 等待节点复位 (OTA_NODE_RESET_WAIT_MS)
 *     3. 从 URL 下载固件到 LittleFS 临时文件
 *     4. 按 Xmodem 协议分包(128B + CRC16) 发送给节点
 *     5. 处理 ACK/NAK/重传
 *     6. 发 EOT 结束传输
 *
 * 调用方式:
 *   - loop() 中周期调用 ota_tick() 驱动状态机
 *   - lora_tick 的 RX 分支中调用 ota_feedByte() 喂 OTA 响应字节
 */
#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>

/* OTA 状态 */
typedef enum {
    OTA_IDLE,               /* 空闲 */
    OTA_TRIGGER_NODE,       /* 发送 AT+OTA 触发节点复位 */
    OTA_WAIT_NODE_RESET,    /* 等待节点复位进 BootLoader */
    OTA_DOWNLOADING,        /* 从网络下载固件到 LittleFS */
    OTA_SENDING,            /* 通过 LoRa 分包发送 */
    OTA_WAIT_ACK,           /* 等待节点 ACK/NAK */
    OTA_RETRY_PACKET,       /* 重发当前包 */
    OTA_SEND_EOT,           /* 发送传输结束标志 (第1个EOT, ⭐ S26 双EOT确认) */
    OTA_SEND_EOT2,          /* ⭐ S26: 发送第2个EOT (连续两个0x04节点才确认结束) */
    OTA_WAIT_FINAL_ACK,     /* 等待最终 CRC32 验证 ACK */
    OTA_COMPLETE,           /* OTA 完成 */
    OTA_FAILED,             /* OTA 失败 */
    OTA_RETRY_WAIT          /* ⭐ 整链重试等待: 失败后停 OTA_CHAIN_RETRY_INTERVAL_MS
                              再整链重来(从 OTA_TRIGGER_NODE 重新发起) */
} OtaState_t;

/* OTA 进度信息 */
typedef struct {
    OtaState_t  state;          /* 当前状态 */
    uint8_t     nodeId;         /* 目标节点 ID */
    uint32_t    totalBytes;     /* 固件总大小 */
    uint32_t    sentBytes;      /* 已发送字节数 */
    uint8_t     retryCount;     /* 当前包重试次数 */
    uint32_t    elapsedMs;      /* 已用时间(ms) */
    char        version[16];    /* 固件版本号 */
} OtaProgress_t;

/* 初始化 OTA 处理器 */
void ota_init(void);

/* 触发 OTA: 向 nodeId 节点发送 url 指向的固件, version 格式 "V2.2"
 * 返回 true 表示启动成功(已在空闲状态), false 表示 OTA 忙 */
bool ota_start(uint8_t nodeId, const char *url, const char *version);

/* 触发 OTA: 固件已下载到 OTA_FW_FILE, 跳过下载阶段直接 LoRa 下发
 * (供 OneNET 平台 OTA 客户端使用), version 为平台任务的目标版本 */
bool ota_startFromFile(uint8_t nodeId, const char *version);

/* ⭐ S14/8.1 统一判定器: 节点 mode=BOOT(收到 PONG,BOOT)时调用, 单一决策:
 *   - ota_getState() != OTA_IDLE → 不动作 (升级进行中, 现有机制接管)
 *   - OTA_IDLE && OTA_FW_FILE 存在 → ota_startFromFile 自动重发 (自愈)
 *   - OTA_IDLE && 无固件           → 置节点 bootPending=true, 等平台下发
 * 自动(节点自曝)/人工/平台 触发全部汇入本入口与 ota_start 单轨. */
bool ota_autoDispatch(uint8_t nodeId);

/* ⭐ S14 人工触发统一入口 (串口 OTA 命令不再直接调 ota_start):
 * 与自动路径同轨 - 节点已在 Boot 且网关有固件 → 判定器文件自愈重发;
 * 否则 → ota_start 下载 URL 固件并触发 (OTA 忙时拒绝, 防双轨并发) */
bool ota_manualTrigger(uint8_t nodeId, const char *url, const char *version);

/* 主循环周期调用, 驱动 OTA 状态机 */
void ota_tick(void);

/* 喂 OTA 响应字节 (从 LoRa RX 状态机调用) */
void ota_feedByte(uint8_t b);

/* 获取当前 OTA 进度 */
const OtaProgress_t *ota_getProgress(void);

/* 获取当前状态 */
OtaState_t ota_getState(void);

/* 取消 OTA */
void ota_cancel(void);

/* ⭐ 通知 OTA 处理器: 节点已回复 AT+OTA:ack 触发确认
 * 由 lora_handler 在收到节点 ACK 帧时调用, 触发命令可靠投递.
 * ⭐ P1-5: nodeId 用于校验确认必须来自最近触发命令的目标节点,
 * 防止多节点并存时他节点 ACK 被误当本节点触发确认 (历史日志现象) */
void ota_notifyTriggerAck(uint8_t nodeId);

/* ⭐ S21: 通知 OTA 处理器: 节点 App 已回复 AT+OTA:version_ok (拒绝升级).
 * 标记节点 otaRefused=true (待人工/平台介入), 终止对 App 的 Xmodem 流
 * (不产生空口噪声), 上报平台; 仅当人工重触发时经 ota_start 清除标记 */
void ota_notifyVersionOk(uint8_t nodeId);

/* ⭐ S15: 通知 OTA 处理器: 节点确认离开 Boot (收到 PONG,APP).
 * 仅当该节点是最近一次 OTA 目标时清理固件文件 (自愈闭环终点) */
void ota_notifyNodeApp(uint8_t nodeId);

#endif /* OTA_HANDLER_H */