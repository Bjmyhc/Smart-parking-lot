/*
 * 智能停车场 - ESP8266/ESP32 网关主程序 v2
 *
 * 架构(方案A: 定点传输 + 二进制帧 + 网关轮询):
 *
 *   STM32节点1 (addr 0x0001) ──┐
 *   STM32节点2 (addr 0x0002) ──┼──LoRa定点──→ ESPx 网关(addr 0x0000) ──WiFi──→ OneNET
 *   STM32节点N (addr N)     ──┘
 *
 * Gateway 核心任务:
 *   1. LoRa 轮询调度:  轮流发 AT+CERx / AT+DATAx
 *   2. 接收二进制帧:  [帧头 0xB1] + 10字节 NodeData  → 更新本地缓存
 *   3. OneNET MQTT:   代子设备上线 + 代子设备上报
 *   4. 下行命令:      OneNET 设置 LedEnable → LoRa 定点帧 AT+LedEnable=X
 *
 * 事件驱动架构(借鉴参考项目):
 *   passiveEvent(): 串口/网络接收到的数据, 立即处理
 *   activeEvent():  定时器触发(轮询/PING/上传), 分时执行
 *
 * 依赖库:
 *   PubSubClient (Nick O'Leary)    MQTT 客户端
 *   ArduinoJson  (Benoit Blanchon) JSON 解析/序列化
 *
 * 开发板:
 *   - NodeMCU 1.0(ESP-12E) / Wemos D1 mini → SoftSerial(LORA_BAUD 请设9600)
 *   - ESP32 / ESP32-S3           → HardwareSerial UART1 (稳定, 115200)
 */

#include <Arduino.h>
#if defined(ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include "config.h"
#include "node_data.h"
#include "lora_handler.h"
#include "onenet_handler.h"
#include "config_portal.h"

/* ==================== 全局变量 ==================== */
uint32_t sysEventFlag = 0;          /* 系统事件标志位 */

static uint32_t lastUpload    = 0;
static uint32_t lastWifiRetry = 0;
static uint32_t lastMqttRetry = 0;
static uint32_t lastPingReq   = 0;

/* 当前使用的 WiFi 配置 (默认取 config.h 宏, 有存档则覆盖) */
static char wifiSsid[33]     = WIFI_SSID;
static char wifiPassword[65] = WIFI_PASSWORD;

/* ==================== WiFi ==================== */
static void wifi_init(void)
{
    /* 长按配网按键 */
    pinMode(CONFIG_KEY_PIN, INPUT_PULLUP);

    /* 读取 Flash 中已保存的 WiFi 配置, 覆盖默认值 */
    WifiConfig_t cfg;
    if (loadWifiConfig(&cfg))
    {
        strncpy(wifiSsid, cfg.ssid, sizeof(wifiSsid) - 1);
        wifiSsid[sizeof(wifiSsid) - 1] = '\0';
        strncpy(wifiPassword, cfg.password, sizeof(wifiPassword) - 1);
        wifiPassword[sizeof(wifiPassword) - 1] = '\0';
        DBG_PRINTF("[WiFi] Using saved config: %s\n", wifiSsid);
    }
    else
    {
        /* 首次启动无配置 → 自动进入配网模式 (保存后自动重启) */
        DBG_PRINTLN("[WiFi] No saved config, entering config portal");
        runConfigPortal();
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    DBG_PRINTF("[WiFi] SSID=%s\n", wifiSsid);
}
static bool wifi_connected(void) { return WiFi.status() == WL_CONNECTED; }

static void wifi_handleReconnect(void)
{
    if (wifi_connected()) return;
    uint32_t now = millis();
    if (now - lastWifiRetry < WIFI_RETRY_DELAY) return;
    lastWifiRetry = now;
    DBG_PRINTLN("[WiFi] Disconnected, retrying...");
    WiFi.reconnect();
}

/* ==================== 主动事件: 定时触发 ==================== */
static void activeEvent(void)
{
    uint32_t now = millis();

    /* 1. 每 60s: MQTT PING 心跳 (借鉴参考项目 PING_SENT 标志位) */
    if ((sysEventFlag & SYS_EVENT_MQTT_CONNECTED) && (now - lastPingReq >= PING_INTERVAL))
    {
        if (sysEventFlag & SYS_EVENT_PING_SENT)
        {
            /* 上次 PING 没收到 PINGRESP → MQTT 掉线 */
            DBG_PRINTLN("[MQTT] PING timeout, disconnecting");
            onenet_disconnect();
            sysEventFlag &= ~(SYS_EVENT_MQTT_CONNECTED | SYS_EVENT_PING_SENT);
        }
        else
        {
            onenet_ping();
            sysEventFlag |= SYS_EVENT_PING_SENT;
            DBG_PRINTLN("[MQTT] PING sent");
        }
        lastPingReq = now;
    }

    /* 2. 每 15s 或 dataChanged: 代子设备上线/下线/数据上报 */
    if (sysEventFlag & SYS_EVENT_MQTT_CONNECTED)
    {
        if (dataChanged || (now - lastUpload >= UPLOAD_INTERVAL))
        {
            onenet_uploadAll();
            lastUpload  = now;
            dataChanged = false;
        }
    }
}

/* ==================== 被动事件: 外部数据到达, 立即处理 ==================== */
static void passiveEvent(void)
{
    /* 1. LoRa 轮询 + 数据接收 (最优先, 每次循环都跑) */
    lora_tick();

    /* 2. 节点超时离线检测 */
    checkNodeTimeout();

    /* 3. MQTT 保活 + 接收下行命令 */
    if (sysEventFlag & SYS_EVENT_MQTT_CONNECTED)
    {
        onenet_loop();
    }
}

/* ==================== setup ==================== */
void setup(void)
{
    DEBUG_SERIAL.begin(DEBUG_BAUD);
    delay(200);
    DBG_PRINTLN("\n=============================");
    DBG_PRINTLN(" Smart Parking Gateway v2.0 ");
    DBG_PRINTLN(" LoRa 定点 + 二进制 + 轮询 ");
    DBG_PRINTLN("=============================");

    lora_init();
    resetAllNodes();
    /* 从 Flash 加载已持久化的子设备证书 (若存在) */
    loadCertsFromLittleFS();
    onenet_init();

    wifi_init();
    WiFi.begin(wifiSsid, wifiPassword);
    uint32_t t0 = millis();
    while (!wifi_connected() && (millis() - t0 < 15000))
    {
        delay(500);
        DBG_PRINT(".");
    }
    if (wifi_connected())
        DBG_PRINTF("\n[WiFi] Connected! IP=%s\n", WiFi.localIP().toString().c_str());
    else
        DBG_PRINTLN("\n[WiFi] Connect failed, will retry in loop");

    if (wifi_connected())
    {
        if (onenet_connect())
        {
            delay(500);
        }
    }
    DBG_PRINTLN("[Gateway] Setup done\n");
}

/* ==================== loop ==================== */
void loop(void)
{
    /* 长按5秒: 进入 AP+Web 重新配网 (阻塞, 保存后重启) */
    checkConfigKeyLongPress();

    /* 被动事件: 数据到达立即处理 */
    passiveEvent();

    /* WiFi 管理 */
    wifi_handleReconnect();

    /* MQTT 连接管理 */
    if (wifi_connected())
    {
        if (!(sysEventFlag & SYS_EVENT_MQTT_CONNECTED))
        {
            uint32_t now = millis();
            if (now - lastMqttRetry >= MQTT_RETRY_DELAY)
            {
                lastMqttRetry = now;
                if (onenet_connect())
                {
                    delay(500);
                    onenet_uploadAll();
                }
            }
        }
        else
        {
            /* 主动事件: 定时任务 */
            activeEvent();
        }
    }
    delay(1);   /* 让 LoRa 软串有时间中断 */
}