/* onenet_ota.h - OneNET Studio 固件升级 (fuse-ota HTTP API) 客户端
 *
 * 功能: 让网关从 OneNET 平台"固件升级"服务自动获取并执行 OTA 任务,
 * 实现"平台发布固件 → 网关自动升级"的完整闭环:
 *
 *   FOTA 任务(type=1, 模组/网关): 下载 ESP8266 固件 → Updater 自升级
 *   SOTA 任务(type=2, MCU/节点):  下载节点固件 → 现有 LoRa OTA 链路转发
 *
 * 流程(周期轮询, 非阻塞):
 *   上报版本 → check 检测任务 → 有任务则下载 → 分发 → 上报进度
 *
 * 依赖: ota_handler (LoRa 下发), platform_cfg (AccessKey/版本)
 */
#ifndef ONENET_OTA_H
#define ONENET_OTA_H

#include <Arduino.h>

/* 初始化 (需在 WiFi 就绪后周期调用 tick) */
void onenet_ota_init(void);

/* 主循环周期调用: 驱动版本上报/任务检测/下载/分发 */
void onenet_ota_tick(void);

/* 当前是否有正在进行的平台 OTA 流程 (含下载) */
bool onenet_ota_busy(void);

#endif /* ONENET_OTA_H */
