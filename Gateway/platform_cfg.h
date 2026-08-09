/* platform_cfg.h - 平台与用户凭据配置
 *
 * 本文件是用户唯一需要修改的配置文件:
 *   - WiFi 名称/密码
 *   - OneNET 网关设备 (PGW001) 鉴权信息 + 网关+子设备模式 Topic
 *   - AP+Web 配网热点参数
 *
 * 说明:
 *   - LoRa 串口/OLED 引脚等硬件配置见 hw_cfg.h
 *   - 轮询/超时/心跳等行为参数见 app_cfg.h
 *   - 协议常量(帧头/结构体)见 lora_protocol.h
 */
#ifndef PLATFORM_CFG_H
#define PLATFORM_CFG_H

/* ==================== WiFi 配置 ==================== */
#define WIFI_SSID           "Guo"
#define WIFI_PASSWORD       "13939695650."

/* ==================== OneNET MQTT 配置 ====================
 * 这里配置的是"网关设备"(PGW001) 自身的鉴权信息.
 * 网关不建物模型! 物模型建在子设备(park1/park2...)上.
 * 网关负责: 代子设备上线(thing/sub/login) + 代子设备上报(thing/pack/post)
 *
 * 平台侧必须配置 (OneNET Studio):
 *   1. 创建网关产品/设备 PGW001 (本产品需支持网关+子设备拓扑)
 *   2. 创建子设备 park1/park2 (可同产品或子产品), 并绑定到 PGW001 拓扑
 *   3. 在 park1/park2 上建物模型属性, 标识符必须与 onenet_handler.cpp
 *      中 SUB_PROP_* 宏完全一致 (ParkStatus/Ultrasonic/GeoMagnetic/
 *      OccupiedTime/LED/LedEnable) */
#define ONENET_SERVER       "9YIs0S7V11.mqtts.acc.cmcconenet.cn"
#define ONENET_PORT         1883
#define ONENET_PROID        "9YIs0S7V11"            /* 产品ID(网关) */
#define ONENET_DEVID        "PGW001"                /* 网关设备名 */
#define ONENET_TOKEN        "version=2018-10-31&res=products%2F9YIs0S7V11%2Fdevices%2FPGW001&et=1815919855&method=md5&sign=uvs3Geaz6%2FvYjQwQ2TGrmw%3D%3D"  /* 网关设备鉴权Token */

/* OneNET 网关+子设备模式 Topic (官方文档"网关与子设备通信Topic")
 * 注意: {device-name} 位置填的是"网关设备名"(PGW001), 不是子设备名!
 *   上线请求:   $sys/{pid}/{网关}/thing/sub/login
 *   批量上报:   $sys/{pid}/{网关}/thing/pack/post   (params 里带子设备 identity)
 *   下行设置:   $sys/{pid}/{网关}/thing/sub/property/set */
#define TOPIC_SUB_LOGIN        "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/sub/login"
#define TOPIC_SUB_LOGIN_REPLY  "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/sub/login/reply"
#define TOPIC_SUB_LOGOUT       "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/sub/logout"
#define TOPIC_PACK_POST        "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/pack/post"
#define TOPIC_PACK_POST_REPLY  "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/pack/post/reply"
#define TOPIC_SUB_SET          "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/sub/property/set"
#define TOPIC_SUB_SET_REPLY    "$sys/" ONENET_PROID "/" ONENET_DEVID "/thing/sub/property/set_reply"

/* ==================== AP+Web 配网配置 ==================== */
#define CONFIG_AP_SSID      "ParkingGateway_Config"  /* 配网热点名 */
#define CONFIG_AP_PASSWORD  "12345678"               /* 配网热点密码(需8位以上) */
#define CONFIG_KEY_LONG_PRESS_MS  3000               /* 长按触发配网(ms) */
#define WIFI_CFG_FILE       "/wifi.cfg"              /* WiFi 配置存储文件 */
/* 配网按键引脚(CONFIG_KEY_PIN)在 hw_cfg.h 中按平台定义 */

#endif /* PLATFORM_CFG_H */
