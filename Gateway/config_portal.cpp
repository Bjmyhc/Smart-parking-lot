/* config_portal.cpp - AP+Web 配网模块实现
 *
 * 功能说明 (参考项目 SmartConfig 的替代):
 *   1. 首次启动: Flash 无 /wifi.cfg 时 自动进入配网模式
 *   2. 配网模式: softAP("ParkingGateway_Config") + DNS 劫持
 *      → 手机连热点 → 浏览器打开任意网址/192.168.4.1
 *      → 页面显示扫描到的 WiFi 列表 + 输入密码 → 保存
 *   3. 保存成功: 写入 LittleFS 后 ESP.restart() 正常联网运行
 *   4. 手动触发: 短按 CONFIG_KEY_PIN (<1s) 触发 LoRa 节点发现;
 *      长按 CONFIG_KEY_PIN (3s) 重新进入配网模式
 *
 * 注意: 页面中的中文全部使用 HTML 实体 (&#x...;), 保证文件纯 ASCII,
 *       任何浏览器/编码环境下都不会出现乱码。
 */
#include "config_portal.h"
#include "platform_cfg.h"  /* CONFIG_AP 热点/密码/WIFI_CFG_FILE/长按时间 */
#include "hw_cfg.h"        /* CONFIG_KEY_PIN */
#include "app_cfg.h"       /* sysEventFlag/DBG */
#include "node_data.h"
#include "onenet_handler.h"
#include "gateway_oled.h"
#include "lora_handler.h"

#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #include <DNSServer.h>
  typedef WebServer MyServer;
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <DNSServer.h>
  typedef ESP8266WebServer MyServer;
#endif
#include <LittleFS.h>

/* ==================== WiFi 配置持久化 ==================== */
#define CFG_MAGIC   0x5A  /* 配置文件有效性标识 */

static void writeWifiConfigToFS(const WifiConfig_t *cfg)
{
    if (!LittleFS.begin()) return;
    File f = LittleFS.open(WIFI_CFG_FILE, "w");
    if (!f) { LittleFS.end(); return; }
    f.write(CFG_MAGIC);
    f.write((uint8_t *)cfg, sizeof(WifiConfig_t));
    f.close();
    LittleFS.end();
}

bool loadWifiConfig(WifiConfig_t *cfg)
{
    if (!LittleFS.begin()) return false;
    if (!LittleFS.exists(WIFI_CFG_FILE))
    { LittleFS.end(); return false; }
    File f = LittleFS.open(WIFI_CFG_FILE, "r");
    if (!f) { LittleFS.end(); return false; }
    if (f.read() != CFG_MAGIC)
    { f.close(); LittleFS.end(); return false; }
    if (f.read((uint8_t *)cfg, sizeof(WifiConfig_t)) != sizeof(WifiConfig_t))
    { f.close(); LittleFS.end(); return false; }
    f.close();
    LittleFS.end();
    cfg->ssid[sizeof(cfg->ssid) - 1] = '\0';
    cfg->password[sizeof(cfg->password) - 1] = '\0';
    return (cfg->ssid[0] != '\0');
}

void saveWifiConfig(const char *ssid, const char *password)
{
    WifiConfig_t cfg = { 0 };
    strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
    if (password) strncpy(cfg.password, password, sizeof(cfg.password) - 1);
    writeWifiConfigToFS(&cfg);
    DBG_PRINTF("[配网] 已保存WiFi: %s\n", cfg.ssid);
}

bool hasSavedConfig(void)
{
    WifiConfig_t cfg;
    return loadWifiConfig(&cfg);
}

/* ==================== 配网 Web 页面 ==================== */

/* 首页: WiFi 列表 + 密码输入 + 实时状态卡
 * 页面中的中文均为 HTML 实体, 保证任何编码环境正常显示 */
static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>&#x667A;&#x80FD;&#x505C;&#x8F66;&#x573A;&#x7F51;&#x5173;&#x914D;&#x7F51;</title>
<style>
  body{font-family:system-ui,sans-serif;max-width:480px;margin:20px auto;padding:0 16px;color:#333}
  h1{font-size:22px;text-align:center}
  .card{background:#f7f8fa;border-radius:10px;padding:16px;margin:12px 0}
  select,input{width:100%;padding:10px;margin:6px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:6px;font-size:16px}
  button{width:100%;padding:12px;margin:10px 0;background:#07c160;color:#fff;border:none;border-radius:6px;font-size:16px}
  .st{font-size:13px;line-height:1.8;color:#666}
  .ok{color:#07c160;font-weight:bold}.bad{color:#e64340;font-weight:bold}
</style>
</head>
<body>
<h1>&#x667A;&#x80FD;&#x505C;&#x8F66;&#x573A;&#x7F51;&#x5173;</h1>
<div class="card">
  <div class="st" id="status">&#x72B6;&#x6001;&#x52A0;&#x8F7D;&#x4E2D;...</div>
</div>
<div class="card">
  <form method="POST" action="/save">
    <select name="ssid">
      __WIFI_OPTIONS__
    </select>
    <input type="password" name="password" placeholder="WiFi &#x5BC6;&#x7801; (&#x5F00;&#x653E;&#x7F51;&#x7EDC;&#x53EF;&#x7559;&#x7A7A;)">
    <button type="submit">&#x4FDD;&#x5B58;&#x5E76;&#x91CD;&#x542F;</button>
  </form>
</div>
<script>
  setInterval(function(){
    fetch('/status').then(r=>r.json()).then(d=>{
      document.getElementById('status').innerHTML =
        'MQTT: ' + (d.mqtt? '<span class="ok">&#x5DF2;&#x8FDE;&#x63A5;</span>' : '<span class="bad">&#x672A;&#x8FDE;&#x63A5;</span>') +
        ' | &#x8282;&#x70B9;: ' + d.nodes + '<br>' +
        '&#x8282;&#x70B9;&#x72B6;&#x6001;: ' + d.nodeInfo + '<br>' +
        '&#x5185;&#x5B58;: ' + d.heap + ' B';
    });
  }, 3000);
</script>
</body>
</html>
)rawliteral";

static MyServer server(80);
static DNSServer dnsServer;

/* 扫描 WiFi 生成 select 选项 */
static String buildWifiOptions(void)
{
    String opt = "<option value=\"\">-- &#x8BF7;&#x9009;&#x62E9; WiFi --</option>";
    int8_t n = WiFi.scanNetworks();
    if (n < 0) n = 0;
    for (int8_t i = 0; i < n; i++)
    {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;   /* 隐藏热点跳过 */
        int rssi = WiFi.RSSI(i);
        String bar;
        if      (rssi > -55) bar = "&#x5F3A;";
        else if (rssi > -70) bar = "&#x4E2D;";
        else                 bar = "&#x5F31;";
        opt += "<option value=\"" + ssid + "\">" + ssid +
               " (" + bar + " " + String(rssi) + "dBm)</option>";
    }
    WiFi.scanDelete();
    return opt;
}

/* 状态 JSON: MQTT/节点/内存 (全 ASCII, 中文用 HTML 实体) */
static String buildStatusJson(void)
{
    String nodeInfo;
    uint8_t onlineCnt = 0;
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (i > 0) nodeInfo += ", ";
        nodeInfo += "node" + String(nodes[i].nodeId);
        nodeInfo += nodes[i].online ? "&#x5728;&#x7EBF;" : "&#x79BB;&#x7EBF;";
        if (nodes[i].online) onlineCnt++;
    }
    if (nodeInfo.length() == 0) nodeInfo = "&#x65E0;";

    String json = "{";
    json += "\"mqtt\":" + String(onenet_connected() ? 1 : 0) + ",";
    json += "\"nodes\":" + String(onlineCnt) + ",";
    json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"nodeInfo\":\"" + nodeInfo + "\"";
    json += "}";
    return json;
}

/* 配网模式主循环 (阻塞式) */
bool runConfigPortal(void)
{
    /* 开启 AP+STA 模式: AP 供手机连接, STA 用于扫描 WiFi */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
    delay(300);
    DBG_PRINTF("[配网] 热点已开启: %s (192.168.4.1)\n", CONFIG_AP_SSID);

    server.on("/", []() {
        String page = FPSTR(HTML_PAGE);
        page.replace("__WIFI_OPTIONS__", buildWifiOptions());
        server.send(200, "text/html; charset=utf-8", page);
    });

    server.on("/status", []() {
        server.send(200, "application/json; charset=utf-8", buildStatusJson());
    });

    server.on("/save", []() {
        if (!server.hasArg("ssid"))
        {
            server.send(400, "text/plain", "missing ssid");
            return;
        }
        String ssid = server.arg("ssid");
        String pwd  = server.hasArg("password") ? server.arg("password") : "";
        if (ssid.length() == 0)
        {
            server.send(400, "text/plain; charset=utf-8", "SSID &#x4E0D;&#x80FD;&#x4E3A;&#x7A7A;");
            return;
        }
        saveWifiConfig(ssid.c_str(), pwd.c_str());
        server.send(200, "text/html; charset=utf-8",
                    "<h3 style='text-align:center;padding-top:80px;font-family:sans-serif'>"
                    "&#x4FDD;&#x5B58;&#x6210;&#x529F;, &#x6B63;&#x5728;&#x91CD;&#x542F;&#x8FDE;&#x63A5; " + ssid + "...</h3>");
        delay(1000);
        ESP.restart();
    });

    server.begin();
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));  /* 劫持域名 → 首页 */

    DBG_PRINTLN("[配网] Web服务已启动, 等待配置...");
    sysEventFlag |= SYS_EVENT_CONFIG_PORTAL;

    /* 阻塞式运行: 期间暂停 LoRa 轮询 / MQTT, 直到保存并重启 */
    while (true)
    {
        dnsServer.processNextRequest();
        server.handleClient();
        oled_refresh();       /* 配网页也刷新 OLED, 显示 AP 名/IP */
        delay(2);
    }

    /* 不会执行到这里 */
    return false;
}

/* ==================== 按键检测 (短按触发发现, 长按触发配网) ==================== */
void checkConfigKeyLongPress(void)
{
    static uint32_t pressStart  = 0;
    static bool     wasPressed  = false;
    static bool     longPressTriggered = false;

    /* 软件消抖: 原始电平连续稳定 CONFIG_KEY_DEBOUNCE_MS 才更新消抖结果,
     * 避免机械按键按下/松开瞬间的电平抖动造成误触发或漏触发 */
    static bool     debounced    = false;   /* 消抖后状态 (按下为 true) */
    static bool     lastRaw      = true;    /* 上次原始电平 (未按下为 true) */
    static uint32_t rawChangeAt  = 0;       /* 原始电平最近一次变化时刻 */

    bool raw = (digitalRead(CONFIG_KEY_PIN) == LOW);  /* 内部上拉, 按下为低 */
    uint32_t now = millis();

    if (raw != lastRaw)
    {
        lastRaw     = raw;
        rawChangeAt = now;
    }
    else if (now - rawChangeAt >= CONFIG_KEY_DEBOUNCE_MS)
    {
        debounced = raw;   /* 电平已稳定, 更新消抖结果 */
    }

    bool pressed = debounced;

    if (pressed && !wasPressed)
    {
        pressStart = now;
        wasPressed = true;
        longPressTriggered = false;
    }
    else if (!pressed && wasPressed)
    {
        /* 释放: 判断时长 */
        uint32_t duration = now - pressStart;
        if (duration < CONFIG_KEY_SHORT_PRESS_MS)
        {
            /* 短按 (3s 内松手): 触发 LoRa 节点发现 + OLED 搜索动画 */
            if (!longPressTriggered)
            {
                lora_triggerDiscovery();
                oled_startManualScan();
            }
        }
        wasPressed = false;
    }

    /* 长按 (5s): 进入配网模式 */
    if (wasPressed && !longPressTriggered && (now - pressStart >= CONFIG_KEY_LONG_PRESS_MS))
    {
        longPressTriggered = true;
        DBG_PRINTLN("[配网] 检测到长按, 进入配网模式");
        runConfigPortal();
    }
}
