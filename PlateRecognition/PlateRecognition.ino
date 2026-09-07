#include "esp_camera.h"
#include "img_converters.h"

// ==================== 配置区 ====================

// 工作模S式: 1=串口转发模式(拍照->串口发JPEG->电脑跑OCR), 0=WiFi直发百度OCR模式(独立运行)
#define SERIAL_BRIDGE_MODE 0

#if !SERIAL_BRIDGE_MODE
// === WiFi直发模式: ESP32-S3独立完成拍照+WiFi+百度OCR ===
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <mbedtls/md5.h>
#include <mbedtls/base64.h>

// Token 模式: 1=硬编码(推荐), 0=动态获取
#define USE_HARDCODED_TOKEN 1

// WiFi 信息
const char* ssid = "Aira";
const char* passwd = "Zdgdzl934395.";

// 百度云 API 凭证
const char* apiKey = "7dTZbfdpp2iLH1YwP9i14YXy";
const char* secretKey = "OPQjMV0AMhtEQP5Xek2qr16WQ9jeSV50";

// 硬编码 Token (有效期 30 天, 2026-09-13 过期)
const char* HARDCODED_TOKEN = "24.dad2e22d9f8fd83f3935dc460bdff3f7.2592000.1789306747.282335-124141210";

// 百度 OCR 地址
const char* OCR_HOST = "aip.baidubce.com";
const char* OCR_PATH = "/rest/2.0/ocr/v1/license_plate";

// 重试配置
#define OCR_RETRY_MAX 2       // OCR 请求最多重试次数
#define TLS_TIMEOUT_MS 30000  // TLS 握手超时 (毫秒)

// ==================== OneNET 云平台配置 ====================
// 新产品: ParkCamera, 设备: Cam001 (手持式取证终端)
// 接入方式: MQTT 产品专属接入点 (Studio官方为每个MQTT产品分配独立MQTT域名, 产品创建时协议必须选MQTT)
//           格式: {产品ID}.mqtts.acc.cmcconenet.cn, 与网关 PGW001 的 9YIs0S7V11.mqtts.acc.cmcconenet.cn 完全一致
// Topic: $sys/{pid}/{device_name}/thing/property/post  OneJSON格式
// 优势: 设备主动上报语义, r/rw属性均可写入, 一次连接多次publish
#define ONENET_ENABLE 1       // 1=开启OneNET结果上报, 0=只串口打印不上云
#define ONENET_PID      "4enONCu0Y7"                     // ParkCamera新产品ID (已选MQTT协议)
#define ONENET_DEVNAME  "Cam001"                          // 设备名
// MQTT Broker: 新产品专属域名 (平台显示 4enONCu0Y7.mqtts.acc.cmcconenet.cn)
#define ONENET_MQTT_HOST  ONENET_PID ".mqtts.acc.cmcconenet.cn"
#define ONENET_MQTT_PORT  1883                            // 明文MQTT(8883=TLS, 需证书)
// 设备级token (token_gen.py生成, MD5, 150天有效期)
// res=products/4enONCu0Y7/devices/Cam001, key=QzA1dVVoc1BiSU1tREhFVjBxT0tvQWlCQTY3SWtSTnQ=
#define ONENET_MQTT_TOKEN  "version=2018-10-31&res=products%2F4enONCu0Y7%2Fdevices%2FCam001&et=1800872330&method=md5&sign=MCwUW5ZeMfsP%2FnVYfs55hQ%3D%3D"
// OneNET Studio MQTT规范(同网关onenet_handler.cpp:525):
//   clientId = 设备名(Cam001),  username = 产品ID(4enONCu0Y7),  password = 设备级token
#define ONENET_MQTT_USER     ONENET_PID
#define ONENET_MQTT_CLIENTID ONENET_DEVNAME
// 属性上报Topic: $sys/{pid}/{device_name}/thing/property/post
#define ONENET_TOPIC_POSTPROP  "$sys/" ONENET_PID "/" ONENET_DEVNAME "/thing/property/post"
// 属性上报回执Topic: $sys/{pid}/{device_name}/thing/property/post/reply  (平台在此回 code=200/错误码)
#define ONENET_TOPIC_POSTPROP_REPLY  "$sys/" ONENET_PID "/" ONENET_DEVNAME "/thing/property/post/reply"
// 拍照触发服务(TriggerCapture): 平台 call-service 同步调用 → 设备订阅/回复下面 topic
//   (替换原 CaptureCmd 读写属性: 拍照触发由属性下发迁移为服务调用)
#define ONENET_SERVICE_TRIGGER   "TriggerCapture"   // 服务identifier, 必须与物模型一致
// MQTT 服务 invoke 订阅: 通配+, 支持以后扩展其他服务 (不可含#多段通配)
#define ONENET_TOPIC_SERVICE_INVOKE  "$sys/" ONENET_PID "/" ONENET_DEVNAME "/thing/service/+/invoke"
// MQTT 服务 reply 主题模板: snprintf 注入 serviceId, 格式同网关 sub 服务 reply 机制
#define ONENET_TOPIC_SERVICE_REPLY_FMT "$sys/" ONENET_PID "/" ONENET_DEVNAME "/thing/service/%s/invoke_reply"
// OneNET上报DeviceStatus枚举值
#define DEVSTAT_IDLE      0   // 空闲(平台枚举值0: 启动/会话结束后上报, APP下发闸门=0)
#define DEVSTAT_SUCCESS   1   // 识别成功(平台枚举值1: 5属性包内携带, 结果态)
#define DEVSTAT_FAIL      2   // 识别失败(平台枚举值2: 失败出口单独上报, 结果态)
#endif

// === 串口帧协议 (SERIAL_BRIDGE_MODE=1 时生效) ===
// 帧格式: "$IMG,<len>\n" + <JPEG 二进制数据 len 字节> + <CRC32 4 字节大端> + "\n$END\n"
// 接收端(Python)按行读 header, 解析 len, 然后精确读取 len+4 字节并校验 CRC32

// ==================== 摄像头引脚 (ESP32-S3-EYE) ====================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

#define BOOT_BTN_PIN 0  // GPIO0 = 板载 BOOT 按钮

// ==================== 全局变量 ====================
bool lastBtnState = HIGH;
bool recognizeInProgress = false;
// 拍照识别会话序号 (每次触发+1, 用于日志把一整个流程串联起来, 避免穿插时分不清)
static uint32_t sessionSeq = 0;
// TriggerCapture 服务调用待执行: MQTT回调只登记(msgId/serviceId), 主循环拾取跑完识别后统一回 invoke_reply
// ⚠️ 回调内不能直接跑识别流程: recognizePlate 内部 uploadToOneNET/wait 会再调 mqttClient.loop() → 回调重入
volatile bool captureSvcPending = false;
char captureSvcMsgId[32] = { 0 };   // 服务调用msgId (回调写, 主循环读)
char captureSvcId[24]    = { 0 };   // 服务identifier
// ⭐ 最近一次 TriggerCapture 识别的 OCR 是否真正识别出车牌 (成功=true, 失败/无牌=false)
// 供主循环统一回 invoke_reply 时把 Result 填成真实识别结果(不再恒定1), App 据此判断要不要拉属性取牌
static bool srvCapturePlateOk = false;
#if !SERIAL_BRIDGE_MODE
DynamicJsonDocument document(8192);
const char base64Map[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
// MQTT 客户端 (OneNET属性上报专用)
WiFiClient mqttWifiClient;
// ---------- msgId↔会话号映射 + 回执等待机制 (日志穿插优化A+B) ----------
// 映射表: 环形缓冲存最近32条 publish msgId → sessionSeq, 回执时反查打[#N]前缀
#define MSG_SESSION_MAP_SIZE 32
static struct { uint32_t msgId; uint32_t sess; } msgSessionMap[MSG_SESSION_MAP_SIZE];
static uint8_t  msgSessionWrIdx = 0;
// 未收回执计数: publish+1, 收到回执-1, 会话结束前wait清零
static volatile uint8_t mqttPendingReplies = 0;
static int mqttLastReplyCode = -1;    // 最近一次回执code(>=0平台, <=-1000投递失败): callback写 wait读
static unsigned long mqttWaitStartMs = 0;  // wait入口记录开始时间: 用于"耗时"打印
static void mapMsgToSession(uint32_t msgId, uint32_t sess) {
  msgSessionMap[msgSessionWrIdx].msgId = msgId;
  msgSessionMap[msgSessionWrIdx].sess  = sess;
  msgSessionWrIdx = (msgSessionWrIdx + 1) % MSG_SESSION_MAP_SIZE;
}
static uint32_t lookupSessionByMsg(uint32_t msgId) {
  for (uint8_t i = 0; i < MSG_SESSION_MAP_SIZE; i++) {
    if (msgSessionMap[i].msgId == msgId && msgSessionMap[i].sess != 0) return msgSessionMap[i].sess;
  }
  return 0;
}
// ---------- MQTT下行回调: 接收平台post/reply回执 + 服务调用(TriggerCapture拍照触发) ----------
// ⚠️  post/reply 回执才是唯一可信的"平台业务处理结果", 只有 code==200 才能打✅入库成功.
//     publish() 返回 true 仅表示已投递给MQTT/TCP层, 不等于平台真正写入属性.
// 前向声明 (实现见下方 #if !SERIAL_BRIDGE_MODE 工具函数区, 回调/主循环都会调用)
static void srvTriggerReply(const char* msgId, const char* serviceId, int result, int actual);
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  payload[length] = 0;
  // --- 1. property/post/reply: 针对某次属性上报的业务回执 (一一对应msgId) ---
  if (strstr(topic, "thing/property/post/reply")) {
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, (char*)payload);
    if (err) { Serial.printf("[MQTT] post/reply解析失败: %s\n", err.c_str()); return; }
    const char* idStr = doc["id"] | "?";
    uint32_t idNum    = strtoul(idStr, NULL, 10);
    int code          = doc["code"] | -999;
    const char* msg   = doc["msg"] | "";
    uint32_t sess = lookupSessionByMsg(idNum);
    char tag[16]; tag[0] = 0;
    if (sess) snprintf(tag, sizeof(tag), "[#%u] ", sess);
    if (mqttPendingReplies > 0) mqttPendingReplies--;
    mqttLastReplyCode = code;   // callback写code → 等wait函数统一打"响应: code=X, 耗时"
    (void)tag; (void)idStr; (void)msg;  // 新格式不单独打回执, 统一由wait输出
    return;
  }
  // --- 2. thing/service/+/invoke: 平台 call-service 同步服务调用 (TriggerCapture 远程拍照) ---
  if (strstr(topic, "/thing/service/") && strstr(topic, "/invoke")) {
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, (char*)payload);
    if (err) { Serial.printf("[MQTT] service/invoke 解析失败: %s\n", err.c_str()); return; }
    const char* msgId = doc["id"] | "";
    // 从topic抽出服务identifier (service/ 与 /invoke 之间的段)
    char serviceId[24]; serviceId[0] = 0;
    const char* p = strstr(topic, "/thing/service/");
    if (p) {
      p += 15;  // 跳过 "/thing/service/"
      const char* q = strstr(p, "/invoke");
      if (q && (q - p) < (int)sizeof(serviceId)) { memcpy(serviceId, p, q - p); serviceId[q - p] = 0; }
    }
    Serial.printf("[MQTT] 服务调用: %s (msgId=%s)\n", serviceId, msgId);
    if (strcmp(serviceId, ONENET_SERVICE_TRIGGER) != 0) {
      Serial.printf("[MQTT] 服务调用: 未支持服务, 忽略\n");
      return;
    }
    /* 正在识别(如BOOT/服务触发中): 立即回 Result=0 (设备忙) */
    if (recognizeInProgress) {
      srvTriggerReply(msgId, serviceId, 0, 0);
      return;
    }
    /* 空闲: 登记待执行, 主循环跑完流程后统一回 Result=1 */
    strncpy(captureSvcMsgId, msgId, sizeof(captureSvcMsgId) - 1);
    strncpy(captureSvcId, serviceId, sizeof(captureSvcId) - 1);
    captureSvcPending = true;
    Serial.println("[MQTT] 已排队, 下一轮loop执行拍照并回 invoke_reply");
    return;
  }
}
PubSubClient mqttClient(mqttWifiClient);
// 增大MQTT发送缓冲区 (PubSubClient默认256B, OneJSON含中文车牌需要~1KB)
#define MQTT_PACKET_BUF_SIZE 2048
char mqttTxBuf[MQTT_PACKET_BUF_SIZE];
#endif

// ==================== 函数声明 ====================
void vCameraInit(void);
// trigger: 触发来源描述, 用于会话标题展示 (如"本地 BOOT 按钮"/"云端服务 TriggerCapture")
void recognizePlate(const char* trigger);
#if SERIAL_BRIDGE_MODE
// 串口转发模式: 把 JPEG 通过帧协议发到电脑
void sendImageToSerial(const uint8_t *jpgBuf, size_t jpgLen);
#else
char* base64_encode(char *buf, int len);
// 内部实现: 手动TLS握手 + 15KB分块写body (ESP32 mbedTLS缓存仅16KB, 大body必须分块)
int httpTlsPost(const char* host, const char* path, const char* body, size_t bodyLen,
                const char* extraHeaders, String& response);
// OneNET MQTT: 连接/重连Broker (返回是否已连接)
bool onenetMqttEnsureConnected();
// OneNET MQTT: 把params JSON发布到 property/post 主题 (底层上报入口)
bool onenetMqttPublishProps(const char* paramsJson);
// [改动B1] 阻塞等待所有未收回执(默认3s), 会话结束横幅前调用
uint8_t onenetMqttWaitAllReplies(unsigned long timeoutMs = 3000);
// OneNET: 完整上报一次识别结果(5个属性)
bool uploadToOneNET(const String& plate, const String& color, float confidence,
                    const String& captureTime, int deviceStatus);
// OneNET: 只上报DeviceStatus (流程中状态变化时轻量调用)
bool reportDeviceStatus(int deviceStatus);
// TriggerCapture 服务: 主循环跑完识别后一次性回 invoke_reply (⚠️ 只能回一次)
static void srvTriggerReply(const char* msgId, const char* serviceId, int result, int actual);
// 获取当前时间字符串 "YYYY-MM-DD HH:MM:SS"
String getTimeString();
// 车牌颜色 英文→中文 (在recognizePlate表格化输出、upload中都会调用)
static String plateColorZh(const String& en);
#endif

void setup() {
  Serial.begin(115200);
  Serial.println();
  // [对齐网关风格: Gateway.ino L227-L230] 纯等号分割线 + 简洁标题, 零Unicode/零emoji
  Serial.println("================================");
  Serial.println(" 车牌识别取证终端 v1.0");
  Serial.println(" ESP32-S3 + GC2145 + 百度OCR");
  Serial.println("================================");

  // ---- 硬件/内存 ----
  bool psramOk = ESP.getPsramSize() > 0;
  Serial.printf("[系统] 内存: PSRAM %uKB, 堆 %uKB 可用",
                ESP.getPsramSize() / 1024, ESP.getFreeHeap() / 1024);
  if (!psramOk) Serial.print(" (警告: PSRAM未检测到)");
  Serial.println();
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  vCameraInit();
  Serial.println("[摄像头] 初始化完成: GC2145 VGA RGB565");

#if SERIAL_BRIDGE_MODE
  // 串口转发模式: 无需 WiFi, 直接待命
  Serial.println("[系统] 模式: 串口转发 (拍照→帧协议→电脑OCR)");
  Serial.println("[系统] 就绪: 按BOOT按钮拍照发JPEG");
#else
  // ---- WiFi 连接 (仅做STA+连上, 不立即开UDP/TCP socket) [对齐网关L80 [WiFi]前缀] ----
  WiFi.mode(WIFI_STA);
  Serial.printf("[WiFi] 目标WiFi: %s, 连接中", ssid);
  WiFi.begin(ssid, passwd);
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    // ⚠️ 修复: 首次连接失败不再 return 弃疗 —— return 会跳过下方
    //   mqttClient.setServer/setCallback/setBufferSize 初始化, 导致之后 WiFi 即使恢复
    //   (固件自动重连/热点打开) MQTT 也永远连不上, 表现为"必须重启才能连上".
    //   setServer 等纯配置不依赖 WiFi, 照常执行; 真正建连交给 loop() 启动阶段的懒连接.
    Serial.printf(" 失败 status=%d, 继续初始化(建连交给loop懒重连)\n", WiFi.status());
  }
  WiFi.setAutoReconnect(true);   // ⭐ 断线自动重连(显式开启, 与网关行为对齐)
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" 完成 IP=%s RSSI=%ddBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }

  // ⚠️ 关键时序: WiFi刚得到DHCP/链路起来后, lwIP tcpip_thread 仍在处理 ARP/DNS缓存等初始化,
  //    此时立即调用 UDP(NTP)/DNS(MQTT.connect) 可能触发 udp_new_ip_type 的核心锁 assert.
  yield();
  delay(150);
  for (volatile int i = 0; i < 3; i++) yield();
  (void)WiFi.status();

  // ---- NTP [对齐网关前缀风格: [NTP]] ----
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
  Serial.println("[NTP] 后台同步中");

  // ---- OneNET MQTT [对齐网关L518 [MQTT]前缀] ----
  const char* mode = "WiFi直发 (拍照→百度OCR→串口+OneNET)";
#if ONENET_ENABLE
  mqttClient.setBufferSize(MQTT_PACKET_BUF_SIZE);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setServer(ONENET_MQTT_HOST, ONENET_MQTT_PORT);
  mqttClient.setSocketTimeout(8);
  // ⭐ keepalive 60→30: 断网时底层更快感知链路失效(≤60s内)并打日志,
  //    缩小"云平台已判离线但固件 connected() 仍为真"的静默盲区
  mqttClient.setKeepAlive(30);
  Serial.printf("[MQTT] 产品=%s 设备=%s (OneNET上报启用, loop中连接)\n",
                ONENET_PID, ONENET_DEVNAME);
#else
  Serial.println("[MQTT] OneNET上报关闭 (仅串口打印)");
#endif
  Serial.printf("[系统] 模式: %s\n", mode);
  Serial.println("[系统] 就绪: 按BOOT按钮 / 云端服务 TriggerCapture 触发拍照");
#endif
}

void loop() {
#if ONENET_ENABLE && !SERIAL_BRIDGE_MODE
  mqttClient.loop();  // 维持MQTT心跳与下行包处理 (post/reply回执 + 服务TriggerCapture调用)

  // ---- 一次性启动阶段任务: MQTT懒连接 + NTP后续补等 ----
  //      放在 loop 顶部 = FreeRTOS scheduler 已完整跑起, lwIP 核心锁就绪.
  static bool startupNetInitDone = false;
  if (!startupNetInitDone) {
    static unsigned long ntpDeadline = millis() + 15000UL;  // 最多再等15s让NTP对好
    bool mqttOk = onenetMqttEnsureConnected();  // 真正的 MQTT.connect, 懒连接
    // 等 NTP: 只要还没到 1970 年后, 或者超过最大等待时间就继续
    struct tm tchk;
    bool nowNtpOk = getLocalTime(&tchk, 10) && (tchk.tm_year + 1900 >= 2024);
    if (mqttOk && (nowNtpOk || millis() > ntpDeadline)) {
      if (nowNtpOk) {
        Serial.printf("[系统] 启动完成: NTP=%04d-%02d-%02d %02d:%02d:%02d, MQTT就绪\n",
                      tchk.tm_year+1900, tchk.tm_mon+1, tchk.tm_mday,
                      tchk.tm_hour, tchk.tm_min, tchk.tm_sec);
      } else {
        Serial.println("[系统] 启动完成: MQTT就绪, NTP超时(后台继续同步)");
      }
      // 启动就绪 → 上报一次 IDLE=0, 通知APP闸门: 摄像头已空闲, 可接受下发任务
      reportDeviceStatus(DEVSTAT_IDLE);
      onenetMqttWaitAllReplies();
      startupNetInitDone = true;
    }
    // 启动阶段小延时, 避免这里死循环把 CPU 占满导致 lwIP 后台任务抢不到
    delay(20);
    return;
  }
#endif

  if (recognizeInProgress) return;  // 正在拍照识别中, 忽略按键/命令

  // ---- 运行期自愈 + 可观测性 (断网/掉线不再"静默", 日志能完整还原掉线-恢复全过程) ----
  unsigned long nowMsLoop = millis();

  // --- ① WiFi 看门狗: 断线边沿打日志 + 主动 reconnect (默认自动重连不可靠时自愈) ---
  static bool   wifiWatchInit = false;
  static bool   lastWifiUp = false;
  static unsigned long lastWifiKickMs = 0;
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (!wifiWatchInit) {          // 首轮只建立基线, 不把启动中状态误报为事件
    wifiWatchInit = true;
    lastWifiUp = wifiUp;
    if (!wifiUp) lastWifiKickMs = nowMsLoop;
  } else if (wifiUp != lastWifiUp) {
    if (wifiUp) {
      Serial.printf("[WiFi] 重连成功 (IP=%s RSSI=%ddBm)\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      Serial.printf("[WiFi] 断开 (status=%d), 主动重连中\n", (int)WiFi.status());
      WiFi.reconnect();
      lastWifiKickMs = nowMsLoop;
    }
  }
  lastWifiUp = wifiUp;
  if (!wifiUp && nowMsLoop - lastWifiKickMs >= 10000UL) {   // 未恢复则每10s踢一次
    lastWifiKickMs = nowMsLoop;
    WiFi.reconnect();
  }

  // --- ② MQTT 连接状态边沿 + 空闲5s自动重连 (原逻辑) ---
  //      掉线边沿就是"云平台已离线但此前日志无输出"盲区的出口:
  //      connected() 由底层 keepalive 判定失效后翻转为 false, 这里立即留痕
  static unsigned long lastMqttReconnectMs = 0;
  static bool   mqttStateInit = false;
  static bool   lastMqttUp = false;
  bool mqttUp = mqttClient.connected();
  if (!mqttStateInit) {          // 首轮建立基线, 不把启动期未连接误报为掉线
    mqttStateInit = true;
    lastMqttUp = mqttUp;
  } else if (!mqttUp && lastMqttUp) {
    Serial.printf("[MQTT] 掉线 (keepalive/底层判定链路失效), 进入自动重连\n");
  }
  lastMqttUp = mqttUp;
  if (!mqttUp && nowMsLoop - lastMqttReconnectMs >= 5000UL) {
    lastMqttReconnectMs = nowMsLoop;
    onenetMqttEnsureConnected();   // 内部自带失败退避, 不会高频轰炸
  }

  // --- ③ 空闲心跳: 每10分钟上报一次IDLE, 用平台回执自检链路活性 ---
  //      纯空闲期(无拍照)固件默认零上行, 半开连接(路由器静默丢包)可长期假在线;
  //      心跳让"链路活着"在日志里有周期性铁证, 回执超时则强制断开触发重连
  static unsigned long lastHeartbeatMs = 0;
  if (mqttUp && nowMsLoop - lastHeartbeatMs >= 600000UL) {
    lastHeartbeatMs = nowMsLoop;
    Serial.println("[心跳] 空闲链路自检 (IDLE上报)");
    if (reportDeviceStatus(DEVSTAT_IDLE)) {
      // 最多等1s回执: 收不到说明链路假死, 强制断开让重连逻辑立刻接管
      if (onenetMqttWaitAllReplies(1000) > 0) {
        Serial.println("[心跳] 回执超时, 链路疑假死, 强制断开MQTT触发重连");
        mqttClient.disconnect();
      }
    } else {
      Serial.println("[心跳] IDLE上报投递失败, 交由重连逻辑接管");
    }
  }

  // ----- 触发源1: 平台 TriggerCapture 服务调用 (云平台/APP远程拍照) -----
  //      MQTT回调只登记msgId/serviceId, 这里空闲后拾取执行.
  //      识别流程内部已按成功/失败自行上报 DeviceStatus(→IDLE开闸门),
  //      流程结束后再统一回一次 invoke_reply (Result/ActualValue), 服务闭环.
  if (captureSvcPending) {
    captureSvcPending = false;
    char svcMsgId[32]; memcpy(svcMsgId, captureSvcMsgId, sizeof(svcMsgId)); svcMsgId[sizeof(svcMsgId) - 1] = 0;
    char svcId[24];    memcpy(svcId, captureSvcId, sizeof(svcId));          svcId[sizeof(svcId) - 1] = 0;
    captureSvcMsgId[0] = 0;
    captureSvcId[0]    = 0;
    recognizeInProgress = true;
    srvCapturePlateOk = false;            // 先复位: 失败出口保持 false
    recognizePlate("云端服务 TriggerCapture");
    recognizeInProgress = false;
    // ⭐ Result=本次OCR是否识别出车牌(真值), ActualValue=1(拍照动作确实触发执行)
    //    App 据此: Result=true 才去拉 PlateNumber 属性(此时必为本次车牌), false=本次没识别到牌, 不拉不显示
    srvTriggerReply(svcMsgId, svcId, srvCapturePlateOk ? 1 : 0, 1);
    return;
  }

  // ----- 触发源2: BOOT按钮下降沿 (本地手动拍照) -----
  bool curState = digitalRead(BOOT_BTN_PIN);
  if (lastBtnState == HIGH && curState == LOW) {
    delay(50);
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
      recognizeInProgress = true;
      recognizePlate("本地 BOOT 按钮");
      recognizeInProgress = false;
      while (digitalRead(BOOT_BTN_PIN) == LOW) delay(10);
    }
  }
  lastBtnState = curState;
  delay(10);  // 主循环小延时, 降低CPU占用
}



// 拍照 + 传输/识别车牌
#if SERIAL_BRIDGE_MODE
// === 串口转发模式: 拍照后通过帧协议发到电脑, 由电脑调用百度OCR ===
void recognizePlate(const char* trigger) {
  sessionSeq++;
  // [对齐网关风格: 纯等号分割线 + [会话#N]前缀]
  Serial.println();
  Serial.printf("-------------------------------- [会话#%u] 串口转发 --------------------------------\n", sessionSeq);
  Serial.printf("[系统] 触发来源: %s\n", trigger);

  Serial.printf("[摄像头] 拍照中 (丢弃前10帧稳定曝光)\n");
  // 丢弃前10帧(约1秒), 让传感器自动曝光/白平衡完全收敛
  for (int i = 0; i < 10; i++) {
    camera_fb_t *tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(100);
  }
  // 正式拍照
  camera_fb_t * fb = esp_camera_fb_get();
  if (fb == NULL) {
    Serial.println("[摄像头] 拍照失败");
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  Serial.printf("[摄像头] 拍照完成: %dx%d 原始 %d 字节\n",
                fb->width, fb->height, (int)fb->len);

  // JPEG 编码 (质量60: 平衡体积与识别率)
  Serial.printf("[摄像头] JPEG编码中 (质量=60)\n");
  uint8_t *jpgBuf = NULL;
  size_t jpgLen = 0;
  bool jpeg_ok = frame2jpg(fb, 60, &jpgBuf, &jpgLen);
  esp_camera_fb_return(fb);
  fb = NULL;
  if (!jpeg_ok || jpgBuf == NULL) {
    Serial.println("[摄像头] JPEG编码失败");
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  Serial.printf("[摄像头] JPEG完成: %d 字节\n", (int)jpgLen);

  Serial.printf("[串口] 帧协议发送中\n");
  sendImageToSerial(jpgBuf, jpgLen);
  free(jpgBuf);
  Serial.printf("[串口] 图片已发送, 等待电脑端OCR\n");
  Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
}
#else
// === WiFi直发模式: 拍照后直接调用百度OCR ===
void recognizePlate(const char* trigger)
{
  sessionSeq++;
  unsigned long tSessionStart = millis();
  // [对齐网关风格: 纯横线分割线 + [会话#N]前缀 + [模块]标签, 零Unicode/零emoji]
  Serial.println();
  Serial.printf("-------------------------------- [会话#%u] 车牌识别 --------------------------------\n", sessionSeq);
  Serial.printf("[系统] 触发来源: %s\n", trigger);

  // ---- 拍照 ----
  Serial.printf("[摄像头] 拍照中 (丢弃前10帧稳定曝光)\n");
  // 丢弃前10帧(约1秒), 让传感器自动曝光/白平衡完全收敛
  for (int i = 0; i < 10; i++) {
    camera_fb_t *tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(100);
  }
  // 正式拍照
  camera_fb_t * fb = esp_camera_fb_get();
  if (fb == NULL) {
    Serial.println("[摄像头] 拍照失败");
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  Serial.printf("[摄像头] 拍照完成: %dx%d 原始 %d 字节\n",
                fb->width, fb->height, (int)fb->len);

  // ---- JPEG 编码 ----
  Serial.printf("[摄像头] JPEG编码中 (质量=60)\n");
  uint8_t *jpgBuf = NULL;
  size_t jpgLen = 0;
  // 质量60: 实测识别率≈80, 但JPEG体积约小50%, 发送时间减半
  bool jpeg_ok = frame2jpg(fb, 60, &jpgBuf, &jpgLen);
  esp_camera_fb_return(fb);
  fb = NULL;
  if (!jpeg_ok || jpgBuf == NULL) {
    Serial.println("[摄像头] JPEG编码失败");
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  Serial.printf("[摄像头] JPEG完成: %d 字节\n", (int)jpgLen);

  // ---- Base64 编码 + Token + Body ----
  Serial.printf("[OCR] 预处理: Base64+Token+Body\n");
  char *b64 = base64_encode((char*)jpgBuf, jpgLen);
  free(jpgBuf);
  if (b64 == NULL) {
    Serial.println("[OCR] Base64编码失败");
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  Serial.printf("[OCR] Base64: %u 字符\n", (unsigned)strlen(b64));
#if PRINT_BASE64_TO_SERIAL
  Serial.println("       === BEGIN BASE64 ===");
  {
    size_t b64Len = strlen(b64);
    size_t i = 0;
    while (i < b64Len) {
      size_t n = (b64Len - i >= 64) ? 64 : (b64Len - i);
      char tmp[65];
      memcpy(tmp, b64 + i, n);
      tmp[n] = '\0';
      Serial.println(tmp);
      i += n;
    }
  }
  Serial.println("       === END BASE64 ===");
#endif

  // 获取百度access_token
  String token = "";
#if USE_HARDCODED_TOKEN
  token = String(HARDCODED_TOKEN);
  Serial.printf("[OCR] Token: 硬编码, 长度 %u\n", (unsigned)token.length());
#else
  Serial.printf("[OCR] Token: 动态获取中\n");
  token = getAccessToken();
#endif
  if (token.length() == 0) {
    Serial.println("[OCR] 获取Token失败, 无法识别");
    free(b64);
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }

  // 构建 URL-encoded Body: image=<URL编码的Base64>
  size_t b64Len = strlen(b64);
  size_t encodedLen = 0;
  for (size_t i = 0; i < b64Len; i++) {
    unsigned char c = (unsigned char)b64[i];
    if (c == '+' || c == '/' || c == '=' || c == '?' || c == '#' || c == '&')
      encodedLen += 3;
    else
      encodedLen += 1;
  }
  size_t bodyLen = 6 + encodedLen + 1;
  char *body = (char*)ps_malloc(bodyLen);
  if (body == NULL) {
    Serial.println("[OCR] Body内存分配失败 (PSRAM不足?)");
    free(b64);
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  strcpy(body, "image=");
  char *p = body + 6;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < b64Len; i++) {
    unsigned char c = (unsigned char)b64[i];
    if (c == '+' || c == '/' || c == '=' || c == '?' || c == '#' || c == '&') {
      *p++ = '%'; *p++ = hex[(c >> 4) & 0x0F]; *p++ = hex[c & 0x0F];
    } else {
      *p++ = (char)c;
    }
  }
  *p = '\0';
  free(b64);
  Serial.printf("[OCR] Body构建完成: %u 字节\n", (unsigned)strlen(body));

  // ---- 调用百度 OCR (含重试) ----
  size_t bodySize = strlen(body);
  Serial.printf("[OCR] 请求中 (%u 字节, 最多重试 %d 次)\n",
                (unsigned)bodySize, OCR_RETRY_MAX);
  int httpCode = -1;
  String respBody = "";
  for (int retry = 0; retry <= OCR_RETRY_MAX; retry++) {
    if (retry > 0) {
      Serial.printf("[OCR] 重试 %d/%d, 等待 1s\n", retry, OCR_RETRY_MAX);
      delay(1000);
    }
    // WiFi状态检查与重连
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[WiFi] 断开(status=%d), 重连中\n", WiFi.status());
      WiFi.reconnect();
      int wfRetry = 0;
      while (WiFi.status() != WL_CONNECTED && wfRetry < 20) { delay(500); wfRetry++; }
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WiFi] 重连失败, 跳过本轮\n");
        continue;
      }
      Serial.printf("[WiFi] 重连成功 (RSSI %d dBm)\n", WiFi.RSSI());
    }
    String fullPath = String(OCR_PATH) + "?access_token=" + token;
    Serial.printf("[OCR] 第 %d/%d 次 POST\n", retry+1, OCR_RETRY_MAX+1);
    unsigned long reqStart = millis();
    httpCode = httpTlsPost(OCR_HOST, fullPath.c_str(), body, bodySize, NULL, respBody);
    Serial.printf("[OCR] HTTP %d, 耗时 %lums, 响应 %u 字节\n",
                  httpCode, millis()-reqStart, (unsigned)respBody.length());
    if (httpCode == 200) break;
  }
  free(body);

  if (httpCode != 200) {
    Serial.printf("[OCR] 请求全部失败 (最终 HTTP %d)\n", httpCode);
    Serial.printf("[OCR] WiFi status=%d, RSSI=%d dBm\n", WiFi.status(), WiFi.RSSI());
    if (respBody.length() > 0) Serial.printf("[OCR] 响应体: %s\n", respBody.c_str());
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }

  // ---- 解析识别结果 & 上报 OneNET ----
  DeserializationError jsonErr = deserializeJson(document, respBody);
  if (jsonErr) {
    Serial.printf("[OCR] JSON解析失败: %s\n", jsonErr.c_str());
    Serial.printf("[OCR] 原始响应: %s\n", respBody.c_str());
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }
  // 百度API返回结构: {"words_result":{"number":"京Q06666","color":"blue","probability":[...],"vertexes_location":[...]},"log_id":...}
  JsonObject words = document["words_result"].as<JsonObject>();
  if (!words) {
    Serial.println("[OCR] 未识别到车牌 (words_result为空)");
#if PRINT_BASE64_TO_SERIAL == 0
    Serial.printf("[OCR] 原始响应: %s\n", respBody.c_str());
#endif
#if ONENET_ENABLE
    reportDeviceStatus(DEVSTAT_FAIL);
    onenetMqttWaitAllReplies();
    delay(1000);
    reportDeviceStatus(DEVSTAT_IDLE);
    onenetMqttWaitAllReplies();
#endif
    Serial.printf("------------------------------- [会话#%u] 结束 --------------------------------\n\n", sessionSeq);
    return;
  }

  String plate = words["number"];
  String color = words["color"];
  uint64_t logId = document["log_id"] | 0ULL;
  // probability 是7个float数组, 求平均置信度
  float avgProb = 0.0f;
  JsonArray probs = words["probability"].as<JsonArray>();
  if (probs.size() > 0) {
    float sum = 0;
    for (float p : probs) sum += p;
    avgProb = sum / probs.size();
  }
  JsonArray vertexes = words["vertexes_location"].as<JsonArray>();

  // [网关风格: 单行关键信息, 不画表格]
  if (vertexes.size() >= 4) {
    Serial.printf("[OCR] 识别结果: 车牌=%s, 颜色=%s(原%s), 置信度=%.6f, 位置=(%d,%d)→(%d,%d), logId=%llu\n",
                  plate.c_str(), plateColorZh(color).c_str(), color.c_str(), avgProb,
                  vertexes[0]["x"].as<int>(), vertexes[0]["y"].as<int>(),
                  vertexes[2]["x"].as<int>(), vertexes[2]["y"].as<int>(),
                  (unsigned long long)logId);
  } else {
    Serial.printf("[OCR] 识别结果: 车牌=%s, 颜色=%s(原%s), 置信度=%.6f, logId=%llu\n",
                  plate.c_str(), plateColorZh(color).c_str(), color.c_str(), avgProb,
                  (unsigned long long)logId);
  }
  // ⭐ 能走到这里 = OCR 已成功识别出车牌(失败路径早已 return) → 供 invoke_reply Result 填真值
  srvCapturePlateOk = true;

  // OneNET 上报: 最终5属性(含SUCCESS态=1)打包 → 入库后延时1s(防平台限流) → 补IDLE=0开闸门
#if ONENET_ENABLE
  String capTime = getTimeString();
  uploadToOneNET(plate, color, avgProb, capTime, DEVSTAT_SUCCESS);
  onenetMqttWaitAllReplies();
  delay(1000);   // ← 平台publish节流约1次/s, 两次上报间停1s (同网关UPLOAD_MIN_INTERVAL_MS)
  reportDeviceStatus(DEVSTAT_IDLE);
  onenetMqttWaitAllReplies();
#else
  Serial.println("[MQTT] OneNET上报关闭 (ONENET_ENABLE=0), 仅串口展示");
#endif

  Serial.printf("------------------------------- [会话#%u] 结束 (总耗时 %lums) -------------------------------\n\n",
                sessionSeq, millis() - tSessionStart);
}
#endif



#if SERIAL_BRIDGE_MODE
// ========== CRC32 查表法 (IEEE 802.3 多项式 0xEDB88320, 与 zlib/PNG 一致) ==========
static uint32_t crc32_table[256];
static bool crc32_table_inited = false;
static void crc32_init() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
    }
    crc32_table[i] = c;
  }
  crc32_table_inited = true;
}
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
  if (!crc32_table_inited) crc32_init();
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

// ========== 串口帧协议发送 ==========
// 帧: "$IMG,<len>\n" + <JPEG 二进制 len 字节> + <CRC32 4字节大端> + "\n$END\n"
// 发送策略: 先发帧头(让电脑切换到接收二进制状态), 然后分块 write,
//          每块 4KB, 与 Serial 默认 TX 缓冲配合, 调用 yield() 让 USB CDC 持续吐数据
void sendImageToSerial(const uint8_t *jpgBuf, size_t jpgLen) {
  // 1. 计算 CRC32
  uint32_t crc = crc32_compute(jpgBuf, jpgLen);

  // 2. 发送帧头: "$IMG,<len>\n"  (len 用十进制, 方便人工读串口日志)
  Serial.printf("$IMG,%u\n", (unsigned)jpgLen);

  // 3. 分块发送 JPEG 二进制 (每块 4KB, 避免 Serial 默认缓冲堵住)
  const size_t CHUNK = 4096;
  size_t remaining = jpgLen;
  const uint8_t *p = jpgBuf;
  unsigned long tStart = millis();
  while (remaining > 0) {
    size_t chunk = (remaining > CHUNK) ? CHUNK : remaining;
    size_t written = Serial.write(p, chunk);
    if (written == 0) {
      // TX 缓冲满, 等一下再试
      delay(2);
      continue;
    }
    p += written;
    remaining -= written;
    yield();  // 让 USB CDC 任务继续传输
  }
  Serial.flush();  // 等待 TX FIFO 全部发完

  // 4. 发送 CRC32 (4字节大端, 二进制)
  uint8_t crcBytes[4] = {
    (uint8_t)(crc >> 24),
    (uint8_t)(crc >> 16),
    (uint8_t)(crc >> 8),
    (uint8_t)(crc & 0xFF)
  };
  Serial.write(crcBytes, 4);
  Serial.flush();

  // 5. 发送帧尾
  Serial.println("\n$END");

  Serial.printf("[TX] %u 字节 JPEG + CRC32=0x%08X, 耗时 %lums\n",
                (unsigned)jpgLen, crc, millis() - tStart);
}
#endif







#if !SERIAL_BRIDGE_MODE
char* base64_encode(char* buf,int len)

{

  int retLen = ceil(len*1.0/3*4);

  char *retBuf = (char*)ps_malloc(sizeof(char)*(retLen+4));
  if (!retBuf) return NULL;

  int index=0;

  int currIndex=0;

  int i=0;

  int lastCnt = len%3;

  

  for(i=0;i<(len-lastCnt);i+=3){

    index = ( (buf[i] & 0xFC) >> 2);

    retBuf[currIndex]=base64Map[index];

    index = ( ( (buf[i] & 0x03) <<4) + ( (buf[i+1] & 0xF0) >> 4) );

    retBuf[currIndex+1]=base64Map[index];

    index = ( ( (buf[i+1] & 0x0F) << 2) + ( (buf[i+2] & 0xC0) >> 6) );

    retBuf[currIndex+2]=base64Map[index];

    index = (buf[i+2] & 0x3F);

    retBuf[currIndex+3]=base64Map[index];

    currIndex+=4;

  }

  

  if(lastCnt==1){

    index = ( (buf[i] & 0xFC) >> 2);

    retBuf[currIndex]=base64Map[index];

    index = ( (buf[i] & 0x03) << 4);

    retBuf[currIndex+1]=base64Map[index];

    retBuf[currIndex+2]='=';

    retBuf[currIndex+3]='=';

    currIndex+=4;

  }else if(lastCnt==2){

    index = ( (buf[i] & 0xFC) >> 2);

    retBuf[currIndex]=base64Map[index];

    index = ( ( (buf[i] & 0x03) <<4) + ( (buf[i+1] & 0xF0) >> 4) );

    retBuf[currIndex+1]=base64Map[index];

    index = ( (buf[i+1] & 0x0F) << 2);

    retBuf[currIndex+2]=base64Map[index];

    retBuf[currIndex+3]='=';

    currIndex+=4;

  }

  

  retBuf[currIndex]='\0';

  return retBuf;

}
#endif  // !SERIAL_BRIDGE_MODE



void vCameraInit(void)

{

  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;

  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;

  config.pin_d1 = Y3_GPIO_NUM;

  config.pin_d2 = Y4_GPIO_NUM;

  config.pin_d3 = Y5_GPIO_NUM;

  config.pin_d4 = Y6_GPIO_NUM;

  config.pin_d5 = Y7_GPIO_NUM;

  config.pin_d6 = Y8_GPIO_NUM;

  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;

  config.pin_pclk = PCLK_GPIO_NUM;

  config.pin_vsync = VSYNC_GPIO_NUM;

  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;

  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;

  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  config.pixel_format = PIXFORMAT_RGB565;

  config.frame_size = FRAMESIZE_VGA;     // 640x480, 适合车牌识别

  config.fb_count = 2;

  config.fb_location = CAMERA_FB_IN_PSRAM;

  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;



  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {

    Serial.printf("摄像头初始化失败: 0x%x\r\n", err);

    return;

  }



  sensor_t * s = esp_camera_sensor_get();

  if (s == NULL) {

    Serial.println("摄像头传感器为空!");

    return;

  }



  Serial.printf("摄像头型号: 0x%x\r\n", s->id.PID);



  // === 图像质量调节 ===

  s->set_brightness(s, 0);    // 亮度默认

  s->set_contrast(s, 1);      // 对比度+1, 车牌字符更清晰

  s->set_saturation(s, 0);    // 饱和度默认

  // 保持自动曝光和自动白平衡开启, 让传感器自动调节

  s->set_exposure_ctrl(s, 1);

  s->set_whitebal(s, 1);

  // 最大增益限制到4倍, 减少噪点

  s->set_gainceiling(s, GAINCEILING_4X);



  // GC0308/GC032A 需要镜像/翻转处理

  if (s->id.PID == GC0308_PID) {

    s->set_hmirror(s, 0);

  } else if (s->id.PID == GC032A_PID) {

    s->set_vflip(s, 1);

  } else if (s->id.PID == OV2640_PID || s->id.PID == OV3660_PID) {

    s->set_vflip(s, 1);

  }

}


#if !SERIAL_BRIDGE_MODE
// ========== 内部传输: HTTPS POST (自动TLS握手 + 15KB分块写body) ==========
// ESP32的mbedTLS写缓冲只有16KB, 超过必须分块写. 此函数封装所有细节, 调用者只需一行.
// extraHeaders: 额外的请求头字符串(以\r\n结尾, 无需\r\n\r\n), 不需要传NULL
// 返回值: HTTP状态码(如200), 负数=底层错误(-1连接超时, -2写入卡死, -3解析失败)
int httpTlsPost(const char* host, const char* path, const char* body, size_t bodyLen,
                const char* extraHeaders, String& response) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);

  // 1. TLS握手 (保持30s超时, 弱网下必须留够时间)
  unsigned long t0 = millis();
  if (!client.connect(host, 443, TLS_TIMEOUT_MS)) {
    client.stop();
    return -1;
  }
  Serial.printf("[TLS] 握手耗时 %lums\n", millis() - t0);

  // 2. 一次性组装并发送HTTP请求头 (避免多次小write各自触发TLS加密)
  String header;
  header.reserve(512);
  header += "POST ";
  header += path;
  header += " HTTP/1.1\r\nHost: ";
  header += host;
  header += "\r\nContent-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\nContent-Length: ";
  header += (int)bodyLen;
  header += "\r\nConnection: close\r\n";
  if (extraHeaders != NULL) {
    header += extraHeaders;   // 调用方附加的请求头 (如Authorization: ...)
  }
  header += "\r\n";           // 头结束空行
  client.print(header);
  client.flush();

  // 3. 分块写body (15KB分块, 接近mbedTLS 16KB record上限, 减少加密调用次数)
  //    历史教训: 16KB是ESP32 mbedTLS固定SSL缓冲, 分块必须<16KB才能稳
  const uint8_t* bp = (const uint8_t*)body;
  size_t remaining = bodyLen;
  const size_t CHUNK = 15360;  // 15KB, 接近mbedTLS 16KB上限, 减少TLS加密调用次数
  int failCount = 0;
  unsigned long bStart = millis();
  size_t sentPrev = 0;
  unsigned long tPrev = bStart;
  while (remaining > 0) {
    size_t chunk = (remaining > CHUNK) ? CHUNK : remaining;
    size_t written = client.write(bp, chunk);
    if (written == 0) {
      failCount++;
      if (failCount > 500) { client.stop(); return -2; }
      delay(2);  // TCP窗口满通常只等几毫秒, 20ms过于保守浪费时间
      continue;
    }
    failCount = 0;
    bp += written;
    remaining -= written;
    // 每10KB打印一次进度, 观察发送是否真的有进展
    if ((bodyLen - remaining - sentPrev) >= 10240) {
      unsigned long now = millis();
      size_t chunkSent = bodyLen - remaining - sentPrev;
      float speed = chunkSent * 1000.0f / (now - tPrev) / 1024.0f;
      Serial.printf("[Body] 进度 %d/%d bytes (%.1f%%), 本段%.1f KB/s\n",
                    (int)(bodyLen - remaining), (int)bodyLen,
                    (float)(bodyLen - remaining) * 100.0f / bodyLen, speed);
      sentPrev = bodyLen - remaining;
      tPrev = now;
    }
    yield();
  }
  Serial.printf("[Body] 发送 %d bytes, 耗时 %lums\n", (int)bodyLen, millis() - bStart);

  // 4. 读取响应 (用 read()+concat() 代替 readString(), 避免30秒流超时空等)
  //    策略: 总超时30s兜底 + 数据静止2s提前退出 (响应分块返回时, 数据流若静止2秒认为已读完)
  response = "";
  response.reserve(4096);
  uint8_t rbuf[1024];
  unsigned long rStart = millis();
  unsigned long lastData = millis();
  while (client.connected() || client.available()) {
    int avail = client.available();
    if (avail > 0) {
      int toRead = (avail < (int)sizeof(rbuf)) ? avail : (int)sizeof(rbuf);
      int n = client.read(rbuf, toRead);
      if (n > 0) {
        response.concat((const char*)rbuf, n);
        lastData = millis();
      }
    } else {
      yield();
      if (millis() - lastData > 2000) break;  // 数据静止2秒, 认为响应已读完
    }
    if (millis() - rStart > 30000) break;     // 总超时30秒兑底
  }
  client.stop();
  Serial.printf("[Resp] 读取 %d bytes, 耗时 %lums\n", (int)response.length(), millis() - rStart);

  // 5. 解析HTTP状态码 + 剥离响应头 + 解码chunked
  int code = -3;
  int firstLine = response.indexOf('\n');
  if (firstLine > 0) {
    String sl = response.substring(0, firstLine);
    sl.trim();
    int sp1 = sl.indexOf(' ');
    int sp2 = sl.indexOf(' ', sp1 + 1);
    if (sp1 > 0 && sp2 > sp1) code = sl.substring(sp1 + 1, sp2).toInt();
  }
  int sep = response.indexOf("\r\n\r\n");
  if (sep >= 0) response = response.substring(sep + 4);

  // 判断是否 chunked 编码: 响应头里包含"Transfer-Encoding: chunked"或原始body格式为"HEX\r\n...\r\n0"
  // 简单可靠的判断: 如果首行是纯十六进制数字(可能带;ext), 就是chunked
  {
    String raw = response;
    int nl = raw.indexOf("\r\n");
    if (nl > 0 && nl <= 10) {  // chunk size行很短 (<=0xFFFF, 4位16进制最多带扩展)
      String line1 = raw.substring(0, nl);
      line1.trim();
      bool isHex = line1.length() > 0;
      for (unsigned int i = 0; i < line1.length(); i++) {
        char c = line1.charAt(i);
        if (c == ';') break;               // chunk extension, 停止扫描
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          isHex = false; break;
        }
      }
      if (isHex) {
        // chunked 解码: 读SIZE\r\n, 拼SIZE字节, 重复直到SIZE==0
        String decoded;
        int pos = 0;
        while (pos < (int)raw.length()) {
          int szEnd = raw.indexOf("\r\n", pos);
          if (szEnd < 0) break;
          String szStr = raw.substring(pos, szEnd);
          szStr.trim();
          int semi = szStr.indexOf(';');
          if (semi >= 0) szStr = szStr.substring(0, semi);
          long chunkSz = strtol(szStr.c_str(), NULL, 16);
          pos = szEnd + 2;
          if (chunkSz == 0) break;
          if (pos + chunkSz > (int)raw.length()) {
            decoded += raw.substring(pos);
            break;
          }
          decoded += raw.substring(pos, pos + chunkSz);
          pos += chunkSz + 2;   // 跳过 chunk数据 + \r\n
        }
        response = decoded;
      }
    }
  }
  return code;
}

// ========== OneNET 工具函数 (MQTT版, 符合设备上报语义) ==========
// 接入方式: OneNET Studio MQTT 明文 1883, 设备级token鉴权
// 上报Topic: $sys/{pid}/{device_name}/thing/property/post
// 上报Payload: OneJSON = {"id":"seq","version":"1.0","params":{属性键值对}}
// 优势: r / rw 属性均可写入 (因为是设备主动上报, 不是云端下发设置)
//       连接一次后多次publish共用TCP, 比每次TLS握手的HTTP快得多

// ---------- 确保MQTT Broker已连接 (断线自动重连, 返回true=当前已连接) ----------
// ⚠️ 成功日志保持 1 行; 仅在失败时才打印详细参数 + 排查清单, 避免启动日志刷屏
bool onenetMqttEnsureConnected() {
  if (mqttClient.connected()) return true;
  // ⭐ 节流: WiFi未关联/未获IP 提示最多每3s一条, 避免开机无网时疯狂刷屏.
  //    WL_CONNECTED 只代表802.11关联成功, 未必已DHCP拿到IP(此时TCP连接必失败)
  unsigned long nowMs = millis();
  static unsigned long lastWarnMs = 0;
  bool noWifi = (WiFi.status() != WL_CONNECTED);
  bool noIp   = (WiFi.localIP() == IPAddress(0, 0, 0, 0));
  if (noWifi || noIp) {
    if (nowMs - lastWarnMs >= 3000UL) {
      lastWarnMs = nowMs;
      Serial.println(noWifi ? "[MQTT] WiFi未连接, 跳过MQTT连接"
                            : "[MQTT] WiFi已关联但未获IP, 等待DHCP后重连");
    }
    return false;
  }
  // ⭐ 重连退避: 上一轮两连全失败后至少等5s再发起新一轮,
  //    避免热点/网络刚恢复时高频重试把失败状态"咬死"(持续 state=-2 需重启才恢复)
  static unsigned long lastFailMs = 0;
  if (lastFailMs != 0 && nowMs - lastFailMs < 5000UL) return false;
  // 最多尝试2次, 避免卡死
  for (int attempt = 1; attempt <= 2; attempt++) {
    Serial.printf("[MQTT] 连接 %d/2 → %s:%d (cid=%s)\n",
                  attempt, ONENET_MQTT_HOST, ONENET_MQTT_PORT, ONENET_MQTT_CLIENTID);
    // OneNET Studio MQTT规范(同网关onenet_handler.cpp L525):
    //   clientId=设备名, username=产品ID, password=设备token
    bool ok = mqttClient.connect(
        ONENET_MQTT_CLIENTID,     // client id
        ONENET_MQTT_USER,         // username = 产品ID
        ONENET_MQTT_TOKEN,        // password = 设备级token
        NULL, 0, false, NULL, true
    );
    if (ok) {
      lastFailMs = 0;   // 连接成功, 清除失败退避标记
      // 连接成功立刻订阅下行主题 (属性上报回执 + 服务调用: TriggerCapture远程拍照)
      bool sub1 = mqttClient.subscribe(ONENET_TOPIC_POSTPROP_REPLY);
      bool sub2 = mqttClient.subscribe(ONENET_TOPIC_SERVICE_INVOKE);
      Serial.printf("[MQTT] 已连接, 订阅: post/reply=%d, service/+/invoke=%d\n",
                    sub1 ? 1 : 0, sub2 ? 1 : 0);
      return true;
    }
    int rc = mqttClient.state();
    // PubSubClient state + CONNACK返回码双重打印
    const char* hint = "未知";
    if (rc == MQTT_CONNECTION_TIMEOUT) hint = "连接超时(Broker不通或域名解析失败)";
    else if (rc == MQTT_CONNECTION_LOST) hint = "连接丢失";
    else if (rc == MQTT_CONNECT_FAILED)    hint = "TCP连接失败(Broker域名/端口错)";
    else if (rc == MQTT_DISCONNECTED)       hint = "已断开(TCP未建立)";
    else if (rc == MQTT_CONNECT_BAD_PROTOCOL) hint = "MQTT协议版本错";
    else if (rc == MQTT_CONNECT_BAD_CLIENT_ID) hint = "clientId非法";
    else if (rc == MQTT_CONNECT_UNAVAILABLE) hint = "Broker不可用";
    else if (rc == MQTT_CONNECT_BAD_CREDENTIALS) hint = "用户名/密码/Token鉴权失败";
    else if (rc == MQTT_CONNECT_UNAUTHORIZED) hint = "未经授权(Token签名/产品设备不匹配)";
    Serial.printf("[MQTT] 连接 %d/2 失败 state=%d → %s\n", attempt, rc, hint);
    // ---- 只有失败最后一次才打印详细参数 + 排查清单 (避免刷屏) ----
    if (attempt == 2) {
      Serial.printf("[MQTT] Broker:   %s:%d\n", ONENET_MQTT_HOST, ONENET_MQTT_PORT);
      Serial.printf("[MQTT] clientId: %s\n", ONENET_MQTT_CLIENTID);
      Serial.printf("[MQTT] username: %s\n", ONENET_MQTT_USER);
      Serial.printf("[MQTT] Token(len=%u)前32B: %.32s\n",
                    (unsigned)strlen(ONENET_MQTT_TOKEN), ONENET_MQTT_TOKEN);
      if (rc == MQTT_CONNECT_BAD_CREDENTIALS || rc == MQTT_CONNECT_UNAUTHORIZED) {
        Serial.println("[MQTT] 鉴权失败检查: Broker域名/设备密钥/res格式/Token过期/method/设备存在性");
      }
    }
    // 失败后主动断开, 清理 PubSubClient/WiFiClient 内部残留socket状态,
    // 防止"网络已恢复却持续 state=-2 无法自愈、必须重启"的问题
    mqttClient.disconnect();
    delay(500);
  }
  lastFailMs = millis();   // 记录本轮全失败时刻, 触发 ≥5s 重连退避
  return false;
}

// ---------- 底层: 把 params JSON 包成OneJSON, Publish到 property/post Topic ----------
// paramsJson 是属性键值对字符串, 如 "{\"DeviceStatus\":1,\"PlateNumber\":\"京Q06666\"}"
// 平台要求(同网关onenet_handler.cpp L374): 每个属性必须包成 {"value":xxx} 对象, 不能直接写值
// 最终 OneJSON: {"id":"1","version":"1.0","params":{"PlateNumber":{"value":"京A"},"DeviceStatus":{"value":1}}}
// ⚠️ 本函数 ok=true 仅代表"已成功投递给MQTT底层TCP", 不是平台业务入库成功!
//    平台最终结果必须通过 post/reply 回执的 code==200 确认, 由 mqttCallback 打印"最终结论"行
bool onenetMqttPublishProps(const char* paramsJson) {
  if (!onenetMqttEnsureConnected()) {
    Serial.println("[MQTT] 未连接, 跳过上报");
    return false;
  }
  // 1. 解析 params JSON
  StaticJsonDocument<384> paramsDoc;
  DeserializationError perr = deserializeJson(paramsDoc, paramsJson);
  if (perr) {
    Serial.printf("[MQTT] params解析失败: %s\n", perr.c_str());
    return false;
  }
  // 2. 包装成 OneJSON (每个属性再套一层 {"value":xxx})
  StaticJsonDocument<768> oneDoc;
  static uint32_t msgSeq = 0;
  char idBuf[16];
  snprintf(idBuf, sizeof(idBuf), "%lu", (unsigned long)++msgSeq);
  mapMsgToSession(msgSeq, sessionSeq);
  oneDoc["id"] = idBuf;
  oneDoc["version"] = "1.0";
  JsonObject params = oneDoc.createNestedObject("params");
  for (JsonPair kv : paramsDoc.as<JsonObject>()) {
    const char* key = kv.key().c_str();
    JsonObject valObj = params.createNestedObject(key);
    valObj["value"] = kv.value();   // 原始类型原样透传: string/float/int/bool 全部保留
  }
  // 3. 序列化成字节流 (写到mqttTxBuf)
  size_t payloadLen = serializeJson(oneDoc, mqttTxBuf, MQTT_PACKET_BUF_SIZE);
  if (payloadLen == 0 || payloadLen >= MQTT_PACKET_BUF_SIZE - 1) {
    Serial.printf("[MQTT] OneJSON序列化失败或超限 (len=%u)\n", (unsigned)payloadLen);
    return false;
  }
  // 4. publish 到 MQTT (QoS0) → 新格式不在此打日志, 由上层调用者打"上报:"行 + wait打"响应:"行
  bool ok = mqttClient.publish(ONENET_TOPIC_POSTPROP,
                               (const uint8_t*)mqttTxBuf, payloadLen, false);
  if (!ok) {
    int state = mqttClient.state();
    mqttLastReplyCode = -1000 - state;  // 负偏移标记投递失败, wait函数会打"投递失败"
    if (state != MQTT_CONNECTED) mqttClient.disconnect();
    return false;
  }
  mqttLastReplyCode = -1;
  mqttPendingReplies++;
  mqttClient.loop();
  (void)idBuf; (void)payloadLen;
  return true;
}

// 阻塞等待回执(最多timeoutMs), 等待完成后只打1行"响应: code=X, 耗时Y" (中文字符严格匹配用户格式); 返回=剩余未收
uint8_t onenetMqttWaitAllReplies(unsigned long timeoutMs) {
  mqttWaitStartMs = millis();
  while (mqttPendingReplies > 0 && (millis() - mqttWaitStartMs) < timeoutMs) {
    mqttClient.loop();
    delay(20);
  }
  unsigned long cost = millis() - mqttWaitStartMs;
  if (mqttLastReplyCode >= 0 && mqttPendingReplies == 0) {
    Serial.printf("[MQTT] 响应: code=%d，耗时 %lums\n", mqttLastReplyCode, cost);
  } else if (mqttLastReplyCode <= -1000) {
    Serial.printf("[MQTT] 响应: 投递失败 state=%d，耗时 %lums\n", -(mqttLastReplyCode + 1000), cost);
  } else {
    Serial.printf("[MQTT] 响应: 超时，耗时 %lums\n", cost);
  }
  return mqttPendingReplies;
}

// ---------- TriggerCapture 服务回包: publish 到 thing/service/{id}/invoke_reply ----------
// OneNET 物模型服务回复 (直连设备, 官方 detail/903):
//   {"id":"<平台调用时的msgId>","code":200,"msg":"success","data":{"Result":true,"ActualValue":true}}
// ⚠️ data 直接放服务输出参数的裸值, 不要再套 {"value":...} 包装 ——
//    {"value":x} 是属性上报(property/post)的规则; 服务回复套了会被平台判 response invalid(2502)
// ⚠️ 每次服务调用只能回一次: 主循环执行完识别后回一次; 设备忙时由回调直接回 (Result=0)
// ⚠️ 可能被 mqttCallback 调用, 内部禁止 mqttClient.loop() 防回调重入; publish 直接走底层socket即发出
static void srvTriggerReply(const char* msgId, const char* serviceId, int result, int actual) {
  if (msgId == NULL || msgId[0] == '\0') {
    Serial.println("[MQTT] 服务回包: 缺msgId, 跳过 invoke_reply");
    return;
  }
  if (!onenetMqttEnsureConnected()) {
    Serial.println("[MQTT] 服务回包: MQTT未连接, 无法回 invoke_reply");
    return;
  }
  StaticJsonDocument<384> oneDoc;
  oneDoc["id"] = msgId;
  oneDoc["code"] = 200;
  oneDoc["msg"] = "success";
  JsonObject data = oneDoc.createNestedObject("data");
  data["Result"]      = (result != 0);   // bool出参 → JSON true/false (勿写成数字1)
  data["ActualValue"] = (actual != 0);
  size_t payloadLen = serializeJson(oneDoc, mqttTxBuf, MQTT_PACKET_BUF_SIZE);
  if (payloadLen == 0 || payloadLen >= MQTT_PACKET_BUF_SIZE - 1) {
    Serial.printf("[MQTT] 服务回包: OneJSON序列化失败或超限 (len=%u)\n", (unsigned)payloadLen);
    return;
  }
  char topic[128];
  snprintf(topic, sizeof(topic), ONENET_TOPIC_SERVICE_REPLY_FMT, serviceId);
  Serial.printf("[MQTT] 服务回包: Result=%d, ActualValue=%d (topic=%s)\n",
                result, actual, topic);
  if (!mqttClient.publish(topic, (const uint8_t*)mqttTxBuf, payloadLen, false)) {
    Serial.println("[MQTT] 服务回包: publish失败");
  }
}

// ---------- 轻量: 只上报DeviceStatus (三态: 0空闲/1成功/2失败) ----------
// ⚠️ 调用者: 必须紧跟 onenetMqttWaitAllReplies() → 本函数只打"上报:"行, wait打"响应:"行
bool reportDeviceStatus(int deviceStatus) {
  const char* tag;
  if      (deviceStatus == 0) tag = "0 (空闲)";
  else if (deviceStatus == 1) tag = "1(成功)";
  else if (deviceStatus == 2) tag = "2(失败)";
  else                        tag = "?(未知)";
  Serial.printf("[MQTT] 上报: 状态=%s\n", tag);
  char pJson[64];
  snprintf(pJson, sizeof(pJson), "{\"DeviceStatus\":%d}", deviceStatus);
  return onenetMqttPublishProps(pJson);
}

// 获取本地时间字符串 (ESP32连WiFi后会自动SNTP同步系统时间) — 仅用于串口打印展示
String getTimeString() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  if (now < 1700000000 || !getLocalTime(&timeinfo, 100)) {
    return "1970-01-01 00:00:00";
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buf);
}

// OneNET dateTime 类型专用: 获取毫秒级int64时间戳 (13位, 自1970-01-01 UTC起)
// OneNET物模型dateTime = 毫秒时间戳, 不是格式化字符串 (否则被拒收)
int64_t getTimeStampMs() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;                 // NTP未同步, 返回0
  return (int64_t)now * 1000LL + (millis() % 1000); // 秒×1000 + 毫秒偏移 (足够演示精度)
}

// 车牌颜色英文→中文映射 (百度OCR返回的是英文, 物模型改为string类型后直接上报中文, APP免映射)
static String plateColorZh(const String& en) {
  String l = en; l.toLowerCase();
  if (l == "blue")   return "蓝色";
  if (l == "yellow") return "黄色";
  if (l == "green")  return "绿色";
  if (l == "white")  return "白色";
  if (l == "black")  return "黑色";
  if (l == "red")    return "红色";
  if (l == "gray" || l == "grey") return "灰色";
  return en;   // 未知则原样返回, 避免丢数据
}

// ---------- 完整上报: 5个属性一次写 (类型严格对齐OneNET物模型定义: PlateConfidence=float 0-1, CaptureTime=string) ----------
bool uploadToOneNET(const String& plate, const String& color, float confidence,
                    const String& captureTime, int deviceStatus) {
  Serial.printf("[MQTT] 上报: 车牌=%s, 颜色=%s, 置信度=%.6f, 时间=%s, 状态=1(成功)\n",
                plate.c_str(), plateColorZh(color).c_str(),
                confidence, captureTime.c_str());

  StaticJsonDocument<384> pDoc;
  pDoc["PlateNumber"] = plate;                              // string 车牌号码 (物模型: string 长度16)
  pDoc["PlateColor"] = plateColorZh(color);                 // string 中文颜色 (物模型: string 长度16)
  // ⚠️ PlateConfidence 物模型严格定义为 float 范围 0-1. 直接传置信度原值, 禁止×100.
  //    超出范围会触发平台 code=2253 float over range, 整条publish被拒(含其他属性)
  pDoc["PlateConfidence"].set<float>(confidence);           // float 0~1 (物模型: float 0-1)
  pDoc["CaptureTime"] = captureTime;                        // string 格式化时间 (物模型: string 长度32)
  pDoc["DeviceStatus"] = deviceStatus;                      // int 0~2枚举 (0空闲/1成功/2失败)
  char pJson[384];
  size_t len = serializeJson(pDoc, pJson, sizeof(pJson));
  if (len == 0) {
    Serial.println("[MQTT] params序列化失败, 取消上报");
    return false;
  }
  return onenetMqttPublishProps(pJson);
}
#endif  // !SERIAL_BRIDGE_MODE

