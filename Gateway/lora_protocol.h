/* lora_protocol.h - LoRa 协议头 (网关端薄包装, S11)
 *
 * ⭐ S11 单一事实源: 协议常量/数据结构/CRC16 已迁移到 shared/lora_protocol.h,
 *    本文件只 include 共享头 + 保留网关侧 OTA 调度参数(非协议, 单端配置)。
 *    节点端 bsp_lora.h / boot_cfg.h 也 include 同一共享头, 双端强制一致。
 *
 * 说明: 本文件不包含任何节点调度参数(那些在 app_cfg.h)。
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-08
 */
#ifndef LORA_PROTOCOL_H
#define LORA_PROTOCOL_H

/* 协议常量/结构体/CRC16 单一事实源
 * ⭐ include 约定: 尖括号 + -I 项目根 (见 Doc/通信架构重构方案.md 编译环境):
 *   - Arduino IDE: esp8266 核心 platform.local.txt 注入 build.extra_flags
 *   - arduino-cli : Tool/build_gateway.ps1 一键脚本自动注入 */
#include <shared/lora_protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 超时(毫秒) ============ */
#define OTA_PACKET_TIMEOUT_MS   2000    /* 每包等待节点回复 */
#define OTA_TOTAL_TIMEOUT_MS    180000  /* 总超时(3分钟) */

/* 每包最大重试次数 */
#define OTA_MAX_RETRY           3

/* 节点复位等待时间(ms): 发送 AT+OTA 收到节点 ACK 后, 节点复位进 BootLoader 需要时间 */
#define OTA_NODE_RESET_WAIT_MS  5000

/* ⭐ AT+OTA 触发命令可靠投递: 发命令后等节点 ACK, 丢包则重发 */
#define OTA_TRIGGER_ACK_TIMEOUT_MS  2000   /* 每次发送后等节点 ACK 的超时(ms) */
#define OTA_TRIGGER_MAX_SEND         3     /* 最多发送次数(含首次, 防 LoRa 丢包) */

/* ⭐ 整链自动重试 (设计文档 3.3.2): OTA_FAILED 不立即放弃,
 * 间隔 OTA_CHAIN_RETRY_INTERVAL_MS 后从 OTA_TRIGGER_NODE 整链重来
 * (重新发 AT+OTA 触发 → 节点复位 → 重新分包发送),
 * 最多 OTA_CHAIN_MAX_RETRY 次(不含首次), 全部失败才回 OTA_IDLE */
#define OTA_CHAIN_MAX_RETRY         3     /* 整链重试次数(不含首次) */
#define OTA_CHAIN_RETRY_INTERVAL_MS 5000  /* 整链重试间隔(ms) */

/* ⭐ S27 自愈预算 (设计文档 15.9): 同一节点"连续自动重发自愈"失败次数上限.
 * 达上限后判定器(ota_autoDispatch)停止自动重发, 只置 bootPending 等平台/人工
 * 显式触发(ota_start / ota_startFromFile 显式入口清零预算).
 * 治理对象: 无脑循环 = 失败耗尽回 OTA_IDLE 后判定器见文件即自动重发, 无限循环 */
#define OTA_SELF_HEAL_MAX_RETRY     3     /* 连续自愈失败上限(次) */

/* OTA 固件临时文件路径 (LittleFS) */
#define OTA_FW_FILE             "/ota_firmware.bin"

#ifdef __cplusplus
}
#endif

#endif /* LORA_PROTOCOL_H */
