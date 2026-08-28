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

/* ⭐ 供 lora_handler 调用: LoRa 阈值下发结果确认后补回"同步服务调用"回复.
 * success=true → Result=1/ActualValue=value; false → Result=0/ActualValue=0 */
void onenet_notifyServiceResult(uint8_t slot, bool success, uint32_t value);

/* ⭐ 向平台查询网关属性 (启动时查询历史阈值配置) */
void sendPropertyGet(const char *attr);

#endif /* ONENET_HANDLER_H */
