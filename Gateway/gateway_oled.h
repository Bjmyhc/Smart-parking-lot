/* gateway_oled.h - 网关 OLED 集中显示模块 (0.96寸 SSD1306, I2C)
 *
 * 显示内容:
 *   - 启动阶段画面: 网络连接中 / 服务器连接中 / 节点扫描中
 *   - 配网模式: AP 热点名 + IP (192.168.4.1)
 *   - 运行模式: WiFi/MQTT 状态 + 各节点车位状态轮播
 *   - 节点 >2 个时自动翻页 (每页2个, 5s/页)
 */
#ifndef GATEWAY_OLED_H
#define GATEWAY_OLED_H

#include <Arduino.h>

/* 初始化 OLED (I2C), 失败不影响主流程 */
void oled_init(void);

/* 启动阶段画面 (setup 按顺序调用):
 *   phase=1: 网络连接中 (WiFi)
 *   phase=2: 服务器连接中 (MQTT)
 *   phase=3: 节点扫描中 (进入扫描画面, 计时从主循环起算, 退出后进主界面) */
void oled_showStartupPhase(uint8_t phase);

/* setup 末尾调用: 开启启动扫描节点画面并开始计时 (主循环起算) */
void oled_scanStart(void);

/* 周期刷新显示 (内部计时, 非阻塞) */
void oled_refresh(void);

#endif /* GATEWAY_OLED_H */
