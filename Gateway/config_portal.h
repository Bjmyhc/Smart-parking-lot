/* config_portal.h - AP+Web 配网模块 (替代参考项目 SmartConfig)
 *
 * 功能:
 *   1. WiFi 配置持久化 (LittleFS 存储 /wifi.cfg)
 *   2. 无配置时自动进入配网模式 (softAP + Web 页面)
 *   3. 长按按键触发重新配网
 *   4. 配网页面显示设备状态 (节点在线 / MQTT 状态 / WiFi 信号)
 */
#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include <Arduino.h>

/* WiFi 配置结构体 (持久化到 LittleFS) */
typedef struct {
    char ssid[33];        /* WiFi 名称 */
    char password[65];    /* WiFi 密码 */
} WifiConfig_t;

/* 从 Flash 读取 WiFi 配置, 返回 true=已保存过配置 */
bool loadWifiConfig(WifiConfig_t *cfg);

/* 保存 WiFi 配置到 Flash */
void saveWifiConfig(const char *ssid, const char *password);

/* 是否有已保存的 WiFi 配置 */
bool hasSavedConfig(void);

/* 进入配网模式 (阻塞式):
 *   - 开启 AP 热点 + DNS 劫持
 *   - Web 页面扫描 WiFi + 输入密码 + 保存
 *   - 保存成功后自动重启
 * 返回: 不会返回 (内部重启); 仅异常时才返回 false */
bool runConfigPortal(void);

/* 长按检测 (需在主循环每轮调用):
 * 按键 CONFIG_KEY_PIN 按下持续 CONFIG_KEY_LONG_PRESS_MS 后触发配网 */
void checkConfigKeyLongPress(void);

#endif /* CONFIG_PORTAL_H */
