/* 智能停车场 - 网关配置总入口
 *
 * 本文件只负责汇总各分层配置文件, 不定义任何宏:
 *   - platform_cfg.h  平台与用户凭据: WiFi / OneNET / Topic / AP配网   (用户必改)
 *   - hw_cfg.h        板级硬件: LoRa串口 / OLED / 按键 / 调试串口      (换板才改)
 *   - app_cfg.h       应用行为: 轮询 / 超时 / 心跳 / OLED扫描 / 事件标志 / DBG
 *   - lora_protocol.h 协议常量: 帧头 / 地址 / 数据结构 (两端人工同步)
 *
 * .ino / .cpp 如需全量配置, 只需 #include "config.h";
 * 更推荐按需 include 对应分层文件。
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "platform_cfg.h"
#include "hw_cfg.h"
#include "app_cfg.h"
#include "lora_protocol.h"

#endif /* CONFIG_H */
