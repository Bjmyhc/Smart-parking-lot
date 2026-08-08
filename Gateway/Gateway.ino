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
 *   3. OneNET MQTT:   聚合所有节点数据 JSON 上报
 *   4. 下行命令:      OneNET 设置 LedEnable → LoRa 定点帧 AT+LedEnable=X
 *
 * 依赖库 (Arduino Library Manager):
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

static uint32_t lastUpload    = 0;
static uint32_t lastWifiRetry = 0;
static uint32_t lastMqttRetry = 0;

/* ---------- WiFi ---------- */
static void wifi_init(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    DBG_PRINTF("[WiFi] SSID=%s\n", WIFI_SSID);
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
    onenet_init();

    wifi_init();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
            /* 刚启动先跑一轮 LoRa 收集数据再上报, 此处先不发 */
        }
    }
    DBG_PRINTLN("[Gateway] Setup done\n");
}

/* ==================== loop ==================== */
void loop(void)
{
    /* 1. LoRa 轮询 + 数据收集 (核心, 最优先) */
    lora_tick();

    /* 2. 节点超时离线 */
    checkNodeTimeout();

    /* 3. WiFi 管理 */
    wifi_handleReconnect();

    /* 4. MQTT 管理 */
    if (wifi_connected())
    {
        if (!onenet_connected())
        {
            uint32_t now = millis();
            if (now - lastMqttRetry >= MQTT_RETRY_DELAY)
            {
                lastMqttRetry = now;
                if (onenet_connect()) { delay(500); onenet_uploadAll(); }
            }
        }
        else
        {
            onenet_loop();
            uint32_t now = millis();
            if (dataChanged || (now - lastUpload >= UPLOAD_INTERVAL))
            {
                onenet_uploadAll();
                lastUpload  = now;
                dataChanged = false;
            }
        }
    }
    delay(1);   /* 1ms 即可, 主要让 LoRa 软串有时间中断 */
}
