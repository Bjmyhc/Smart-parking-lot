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
#include "config.h"                 /* 配置总入口 */
#include "node_data.h"
#include "lora_handler.h"
#include "onenet_handler.h"
#include "config_portal.h"
#include "gateway_oled.h"

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
        DBG_PRINTF("[WiFi] 使用已保存配置: %s\n", wifiSsid);
    }
    else
    {
        /* 首次启动无配置 → 自动进入配网模式 (保存后自动重启) */
        DBG_PRINTLN("[WiFi] 无已保存配置, 进入配网模式");
        runConfigPortal();
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    DBG_PRINTF("[WiFi] 目标WiFi: %s\n", wifiSsid);
}
static bool wifi_connected(void) { return WiFi.status() == WL_CONNECTED; }

static bool     wasWifiConnected  = false;  /* 上一次 WiFi 是否在线 (边沿检测) */
static bool     wifiBeginInFlight = false;  /* begin() 已发起, 连接过程进行中 */
static uint32_t wifiBeginAt       = 0;      /* begin() 发起时刻 */

/* WiFi 状态机上次值: 用于串口输出重连过程 (状态变化才打印一次) */
static wl_status_t lastWifiStatus = WL_IDLE_STATUS;

/* 重连过程中 WiFi.status() 变化 → 串口中文日志 (边沿检测)
 * 注意: 只在未连接状态下调用, WL_CONNECTED 分支理论不会走到 */
static void wifi_printStatusChange(void)
{
    wl_status_t st = WiFi.status();
    if (st == lastWifiStatus) return;
    lastWifiStatus = st;

    const char *msg;
    switch (st)
    {
        case WL_IDLE_STATUS:     msg = "空闲";       break;
        case WL_NO_SSID_AVAIL:   msg = "找不到SSID";  break;
        case WL_SCAN_COMPLETED:  msg = "扫描完成";    break;
        case WL_CONNECTED:       msg = "获取IP中..."; break;
        case WL_CONNECT_FAILED:  msg = "连接失败";    break;
        case WL_CONNECTION_LOST: msg = "连接丢失";    break;
        case WL_DISCONNECTED:    msg = "已断开";      break;
        default:                 msg = "未知状态";    break;
    }
    DBG_PRINTF("[WiFi] 重连过程: %s\n", msg);
}

static void wifi_handleReconnect(void)
{
    if (wifi_connected())
    {
        /* 已连上但 DHCP 还没拿到 IP: 串口展示 GET IP 阶段 */
        if (WiFi.localIP() == (uint32_t)0)
        {
            if (lastWifiStatus != WL_CONNECTED)
            {
                lastWifiStatus = WL_CONNECTED;
                DBG_PRINTLN("[WiFi] 重连过程: 获取IP中...");
            }
        }
        /* 从断线恢复: 打印重连成功日志 (首次连接已在 setup 打印) */
        else if (!wasWifiConnected)
        {
            DBG_PRINTF("[WiFi] 重连成功! IP=%s\n", WiFi.localIP().toString().c_str());
        }
        wasWifiConnected  = true;
        wifiBeginInFlight = false;
        return;
    }
    wifi_printStatusChange();   /* 未连接: 状态变化打到串口 */
    uint32_t now = millis();

    /* WiFi 断线瞬间: MQTT 连接必然失效, 立即清除连接标志.
     * 若不清理, WiFi 恢复后仍认为 MQTT 在线, 不会走重连 → 数据发不出去(假死).
     * 说明: passiveEvent 里的 onenet_loop() 只看 MQTT 标志不看 WiFi,
     *       断线后 mqtt.loop() 多数也能感知底层断开并清标志(OLED 显示空心圆);
     *       这里在边沿瞬间主动清理属于双保险, 覆盖 TCP 半开等
     *       mqtt.loop() 检测慢的边缘场景. */
    if (wasWifiConnected && (sysEventFlag & SYS_EVENT_MQTT_CONNECTED))
    {
        DBG_PRINTLN("[WiFi] 连接丢失, 已清除MQTT标志");
        onenet_disconnect();
    }
    wasWifiConnected = false;

    /* 关键: WiFi.begin() 是异步的, 连接要走 扫描→认证→关联→DHCP, 需数秒.
     * 若每 2s 就 disconnect()/begin(), 会反复打断连接过程导致永远连不上.
     * 因此 begin() 后给 WIFI_CONNECT_TIMEOUT_MS 的连接窗口, 期间不打扰,
     * 超时仍没连上(如 AP 不可用) 才重新发起 */
    if (wifiBeginInFlight)
    {
        if (now - wifiBeginAt < WIFI_CONNECT_TIMEOUT_MS) return;
        wifiBeginInFlight = false;   /* 超时未连上, 落到下面重新 begin */
    }

    if (now - lastWifiRetry < WIFI_RETRY_DELAY) return;
    lastWifiRetry = now;

    wifiBeginInFlight = true;
    wifiBeginAt       = now;
    DBG_PRINTLN("[WiFi] 断开, 强制重连 begin()");
    WiFi.disconnect();
    WiFi.begin(wifiSsid, wifiPassword);
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
            DBG_PRINTLN("[MQTT] 心跳超时, 断开连接");
            onenet_disconnect();
            sysEventFlag &= ~(SYS_EVENT_MQTT_CONNECTED | SYS_EVENT_PING_SENT);
        }
        else
        {
            onenet_ping();
            sysEventFlag |= SYS_EVENT_PING_SENT;
            DBG_PRINTLN("[MQTT] 心跳已发送");
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
    DBG_PRINTLN(" 智能停车场网关 v2.0    ");
    DBG_PRINTLN(" LoRa 定点 + 二进制 + 轮询 ");
    DBG_PRINTLN("=============================");

    oled_init();                       /* OLED 先初始化 (启动阶段画面) */
    lora_init();
    resetAllNodes();
    /* 从 Flash 加载已持久化的子设备证书 (若存在) */
    loadCertsFromLittleFS();
    onenet_init();

    /* --- 阶段1: 网络连接中 (WiFi) --- */
    oled_showStartupPhase(1);
    wifi_init();
    WiFi.begin(wifiSsid, wifiPassword);
    uint32_t t0 = millis();
    while (!wifi_connected() && (millis() - t0 < 15000))
    {
        delay(500);
        oled_refresh();              /* 刷新"网络连接中"画面 */
        wifi_printStatusChange();    /* 首次连接过程状态打到串口 */
        DBG_PRINT(".");
    }
    if (wifi_connected())
    {
        DBG_PRINTF("\n[WiFi] 连接成功! IP=%s\n", WiFi.localIP().toString().c_str());
        wasWifiConnected = true;    /* setup 已连上, 避免 loop 首轮重复打印 */
    }
    else
        DBG_PRINTLN("\n[WiFi] 连接失败, 将在主循环重试");

    /* --- 阶段2: 服务器连接中 (MQTT) --- */
    if (wifi_connected())
    {
        oled_showStartupPhase(2);
        if (onenet_connect())
        {
            delay(500);
        }
    }

    /* --- 阶段3: 节点扫描中 (LoRa) --- */
    oled_showStartupPhase(3);        /* 扫描画面计时从主循环开始 */
    DBG_PRINTLN("[网关] 初始化完成\n");
}

/* ==================== loop ==================== */
void loop(void)
{
    /* 长按5秒: 进入 AP+Web 重新配网 (阻塞, 保存后重启) */
    checkConfigKeyLongPress();

    /* OLED 集中显示刷新 (非阻塞) */
    oled_refresh();

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