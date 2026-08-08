/* 智能停车场 - ESP8266/ESP32 网关配置
 *
 * 注意: WiFi 和 OneNET 配置请用户根据实际修改!
 *       LoRa 配置需与所有节点端 LoRa 模块配置一致
 *       (如果用 SoftSerial, 波特率请改为 9600, 软串高波特率不稳定)
 */
#ifndef CONFIG_H
#define CONFIG_H

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

/* ==================== LoRa 串口配置 ====================
 * - ESP8266(只有1个硬串口): 使用SoftwareSerial
 *     D2 (GPIO4) = LoRa TX -> ESP RX
 *     D1 (GPIO5) = ESP8266 TX -> LoRa RX
 *     LORA_BAUD 软串建议 9600, 115200不稳
 * - ESP32/S3(3个硬串口): 直接 UART1, 引脚自由配置
 *     后续 #if defined(ESP32) 下可切换 HardwareSerial */
#if defined(ESP32)
  #define LORA_USE_HWSERIAL    1
  #define LORA_RX_PIN          4
  #define LORA_TX_PIN          5
  #define LORA_BAUD            115200
#else
  #define LORA_USE_HWSERIAL    0
  #define LORA_RX_PIN          D2
  #define LORA_TX_PIN          D1
  #define LORA_BAUD            9600
#endif

/* ==================== 网关/节点地址 & 轮询参数 ==================== */
/* 包含协议统一常量 (LORA_GATEWAY_ADDR, LORA_POLL_* 等) */
#include "lora_protocol.h"

/* 节点发现: 首次启动后, 网关轮询 AT+CER1~CER<MAX>
 * 收到回复后标记"已注册", 后续改轮询 AT+DATAx
 * POLL_FROM 一般从 1 开始, POLL_TO 按需配置 */
#define LORA_POLL_FROM_NODE         1
#define LORA_POLL_TO_NODE           LORA_MAX_NODES  /* 最大搜索 1..16 号 */

/* ==================== 上报与心跳 ==================== */
#define UPLOAD_INTERVAL     15000   /* 定时上报周期(ms) */
#define HEARTBEAT_INTERVAL  20000   /* MQTT心跳间隔(ms), < OneNET keepalive */
#define NODE_DATA_TIMEOUT   60000   /* 节点超时离线(ms) */
#define MQTT_RETRY_DELAY    5000    /* MQTT重连间隔(ms) */
#define WIFI_RETRY_DELAY    2000    /* WiFi重连间隔(ms) */

/* ==================== 调试输出 ==================== */
#if defined(ESP32)
  #define DEBUG_SERIAL        Serial
#elif LORA_USE_ESP8266_HWSERIAL
  /* 模式2: 调试输出走 UART1 TX (GPIO2/D4), 需额外 USB-TTL 转接 */
  #define DEBUG_SERIAL        Serial1
#else
  /* 模式1: 调试输出走 USB 串口 (Serial), Arduino IDE 串口监视器直接看 */
  #define DEBUG_SERIAL        Serial
#endif
#define DEBUG_BAUD          115200
#define DEBUG_PRINT         1       /* 1=启用, 0=关闭 */

#if DEBUG_PRINT
  #define DBG_PRINT(x)      DEBUG_SERIAL.print(x)
  #define DBG_PRINTLN(x)    DEBUG_SERIAL.println(x)
  #define DBG_PRINTF(f, ...) DEBUG_SERIAL.printf(f, ##__VA_ARGS__)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(f, ...)
#endif

#endif /* CONFIG_H */
