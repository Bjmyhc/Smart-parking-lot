/* onenet_handler.h - Gateway OneNET MQTT 处理 */
#ifndef ONENET_HANDLER_H
#define ONENET_HANDLER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

/* 初始化 (设置MQTT服务器+回调) */
void onenet_init(void);

/* 连接 OneNET (WiFi连接后调用)
 * 返回 true=成功
 * 连接后自动订阅: 子设备上线回复/批量上报回复/子设备属性设置下行 */
bool onenet_connect(void);

void onenet_disconnect(void);

/* loop 周期调用 */
void onenet_loop(void);

bool onenet_connected(void);

/* 周期调用: 代子设备上线/下线 + 批量上报 (网关+子设备模式) */
void onenet_uploadAll(void);

/* 回复平台"子设备属性设置"执行结果 code:200/400 等 */
void onenet_replySet(const char *id, int code, const char *msg);

/* 发送 MQTT PINGREQ 心跳包 (用于 activeEvent 中的 PING_SENT 检测) */
bool onenet_ping(void);

#endif /* ONENET_HANDLER_H */
