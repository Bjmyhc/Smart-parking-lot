/* onenet_handler.cpp - Gateway OneNET MQTT 处理 (网关+子设备模式)
 *
 * 架构(方案B, 借鉴"超子说物联网"阿里云网关参考项目):
 *   - 网关用自身身份(PGW001)连接 OneNET, 网关自身不建物模型
 *   - 物模型建在子设备(park1/park2...)上
 *   - 网关收到节点证书后: 代子设备上线   $sys/{pid}/{gw}/thing/sub/login
 *   - 网关收到节点数据后: 代子设备上报   $sys/{pid}/{gw}/thing/pack/post
 *   - 平台下行控制:        $sys/{pid}/{gw}/thing/sub/property/set
 *
 * OneJSON 报文格式(官方文档 open.iot.10086.cn):
 *   上线: {"id":"1","version":"1.0","params":{"productID":"..","deviceName":".."}}
 *   上报: {"id":"1","version":"1.0","params":[{"identity":{"productID":"..",
 *         "deviceName":".."},"properties":{"ParkStatus":{"value":1},...}}]}
 *   回复: {"id":"1","code":200,"msg":"..."}
 */
#include "onenet_handler.h"
#include "platform_cfg.h"  /* ONENET 服务器/产品/设备/Token/Topic */
#include "app_cfg.h"       /* sysEventFlag/LORA_MAX_NODES/DBG */
#include "node_data.h"
#include "lora_handler.h"
#include "onenet_ota.h"      /* ota_allow_set: App 全网升级确认门控 */
#include <ArduinoJson.h>

/* ==================== 子设备物模型属性标识符 ====================
 * 必须与 OneNET 平台子设备(park1/park2)物模型属性标识符完全一致
 * (名称、大小写都不能错) */
#define SUB_PROP_PARK_STATUS     "ParkStatus"
#define SUB_PROP_ULTRASONIC      "Ultrasonic"
#define SUB_PROP_GEO_MAGNETIC    "GeoMagnetic"
#define SUB_PROP_OCCUPIED_TIME   "OccupiedTime"
#define SUB_PROP_LED             "LED"
#define SUB_PROP_LED_ENABLE      "LedEnable"
/* OTA 全网升级确认: App 下发 OtaAllow=1 后网关才执行已检测到的升级任务.
 * 属性定义在节点产品物模型(下行), 网关在 set 主题按属性名拦截, 不转发节点.
 * OtaProgress: 网关 OTA 实时进度, 上行属性, 供 App 查询驱动精确进度条.
 * 阶段状态改用官方 fuse-ota $tid/check 接口, 不再上报 OtaStatus */
#define SUB_PROP_OTA_ALLOW       "OtaAllow"
#define SUB_PROP_OTA_PROGRESS    "OtaProgress"
/* ⭐ 僵尸车判定阈值(秒): 定义在节点产品物模型上, 可按节点分别设置 */
#define SUB_PROP_ZOMBIE_THRESHOLD "ZombieThresholdSec"
/* ⭐ 僵尸车阈值服务标识符: 定义在节点产品物模型上(非网关).
 * APP 经 call-service 同步调用, 平台转发到网关的 sub/service/invoke 主题,
 * 输入 ThresholdValue, 输出 Result/ActualValue */
#define SUB_SERVICE_ZOMBIE_THRESHOLD "SetZombieThreshold"
/* 同步服务调用截止(ms): 平台同步调用超时约10s, 网关须赶在前面回 invoke_reply */
#define SUB_SERVICE_DEADLINE_MS    9000

/* 代上线等待平台回复的超时(ms), 超时后重新排队 */
#define SUB_LOGIN_TIMEOUT_MS     5000

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

/* 上/下行消息计数 (供 OLED Footer 显示) */
uint32_t mqttTxCount = 0;   /* 网关发往平台的 MQTT 消息数 */
uint32_t mqttRxCount = 0;   /* 平台下发到网关的消息数 */

/* 正在代上线/代下线的节点索引 (OneNET 回复不携带身份,
 * 一次只发一条, 靠 reply 的 id 顺序对应) */
static uint8_t  loginSlot    = 0xFF;
static bool     awaitingLogin = false;
static uint32_t loginSentAt   = 0;

/* ⭐ 子设备服务调用(同步)待回复状态机:
 * 平台同步服务调用 ~10s 内等回复; 网关单线程不能阻塞等 LoRa ACK,
 * 故收到 invoke 后记下待回复状态返回主循环, 等 LoRa ACK 后由
 * onenet_notifyServiceResult() 补回 invoke_reply; 超过截止时间强制回失败 */
static struct {
    bool     active;       /* 是否有待回复的服务调用 */
    char     msgId[32];    /* 平台消息 id, 回复时原样带回 */
    uint8_t  slot;         /* 目标节点索引 */
    uint32_t targetValue;  /* 目标阈值(秒) */
    uint32_t deadlineMs;   /* 截止时间戳(ms) */
} s_pendingServiceReply;

/* ==================== 内部函数 ==================== */

/* 代子设备上线 */
static void subLogin(uint8_t slot)
{
    StaticJsonDocument<256> doc;
    doc["id"] = String(millis());
    doc["version"] = "1.0";
    JsonObject p = doc.createNestedObject("params");
    p["productID"]  = nodes[slot].productKey;
    p["deviceName"] = nodes[slot].deviceName;

    String out;
    serializeJson(doc, out);
    nodes[slot].loginPending = false;
    loginSlot    = slot;
    loginSentAt  = millis();
    awaitingLogin = true;
    DBG_PRINTF("[MQTT] 子设备上线 节点%d (%s/%s): %s\n",
               nodes[slot].nodeId, nodes[slot].productKey,
               nodes[slot].deviceName, out.c_str());
    mqtt.publish(TOPIC_SUB_LOGIN, out.c_str());
    mqttTxCount++;   /* 上行计数 */
}

/* 代子设备下线 */
static void subLogout(uint8_t slot)
{
    StaticJsonDocument<256> doc;
    doc["id"] = String(millis());
    doc["version"] = "1.0";
    JsonObject p = doc.createNestedObject("params");
    p["productID"]  = nodes[slot].productKey;
    p["deviceName"] = nodes[slot].deviceName;

    String out;
    serializeJson(doc, out);
    nodes[slot].logoutPending = false;
    DBG_PRINTF("[MQTT] 子设备下线 节点%d: %s\n", nodes[slot].nodeId, out.c_str());
    mqtt.publish(TOPIC_SUB_LOGOUT, out.c_str());
    mqttTxCount++;   /* 上行计数 */
}

/* 代子设备上报属性 (pack/post) */
static void subPost(uint8_t slot)
{
    NodeData &nd = nodes[slot];

    StaticJsonDocument<1024> doc;
    doc["id"] = String(millis());
    doc["version"] = "1.0";
    JsonArray params = doc.createNestedArray("params");
    JsonObject sub = params.createNestedObject();
    JsonObject identity = sub.createNestedObject("identity");
    identity["productID"]  = nd.productKey;
    identity["deviceName"] = nd.deviceName;
    JsonObject props = sub.createNestedObject("properties");
    props[SUB_PROP_PARK_STATUS]["value"]     = nd.parkStatus;
    props[SUB_PROP_ULTRASONIC]["value"]      = nd.ultrasonic;
    props[SUB_PROP_GEO_MAGNETIC]["value"]    = nd.geoMagnetic;
    props[SUB_PROP_OCCUPIED_TIME]["value"]   = (long)nd.occupiedTime;
    props[SUB_PROP_LED]["value"]             = nd.led;
    props[SUB_PROP_LED_ENABLE]["value"]      = nd.ledEnable;
    props[SUB_PROP_ZOMBIE_THRESHOLD]["value"] = (long)nd.zombieThresholdSec;   /* ⭐ 只读属性: 节点当前生效阈值 */

    String out;
    serializeJson(doc, out);
    DBG_PRINTF("[MQTT] 子设备上报 节点%d: %s\n", nd.nodeId, out.c_str());
    mqtt.publish(TOPIC_PACK_POST, out.c_str());
    mqttTxCount++;   /* 上行计数 */
}

/* 代子设备批量上报属性 (pack/post): 把本轮所有在线且已上线成功的子设备
 * 合并进同一条 params 数组, 整轮仅 1 次上行.
 * 依据 OneNET 平台限制"上行报文 ≤1次/s": 若 N 台节点各自发一条,
 * 同轮 N 条上行会瞬时超限被延迟处理; 合并为一条后单轮只有 1 次上行,
 * 完全符合平台限制. 上线/下线仍逐条处理(需要回复对应身份) */
static void subPostBatch(void)
{
    uint8_t slots[LORA_MAX_NODES];
    uint8_t count = 0;
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].online && nodes[i].subLogin)
            slots[count++] = i;
    }
    if (count == 0) return;

    StaticJsonDocument<2048> doc;   /* 与 MQTT 发送缓冲(2048B)对齐 */
    doc["id"] = String(millis());
    doc["version"] = "1.0";
    JsonArray params = doc.createNestedArray("params");
    for (uint8_t k = 0; k < count; k++)
    {
        NodeData &nd = nodes[slots[k]];
        JsonObject sub = params.createNestedObject();
        JsonObject identity = sub.createNestedObject("identity");
        identity["productID"]  = nd.productKey;
        identity["deviceName"] = nd.deviceName;
        JsonObject props = sub.createNestedObject("properties");
        props[SUB_PROP_PARK_STATUS]["value"]     = nd.parkStatus;
        props[SUB_PROP_ULTRASONIC]["value"]      = nd.ultrasonic;
        props[SUB_PROP_GEO_MAGNETIC]["value"]    = nd.geoMagnetic;
        props[SUB_PROP_OCCUPIED_TIME]["value"]   = (long)nd.occupiedTime;
        props[SUB_PROP_LED]["value"]             = nd.led;
        props[SUB_PROP_LED_ENABLE]["value"]      = nd.ledEnable;
        props[SUB_PROP_ZOMBIE_THRESHOLD]["value"] = (long)nd.zombieThresholdSec;   /* ⭐ 只读属性: 节点当前生效阈值 */
    }

    String out;
    serializeJson(doc, out);
    DBG_PRINTF("[MQTT] 批量上报 %d 台子设备: %s\n", count, out.c_str());
    mqtt.publish(TOPIC_PACK_POST, out.c_str());
    mqttTxCount++;   /* 上行计数 */
}

/* 处理平台下行: 子设备属性设置 -> 转发 LoRa 控制命令 */
static void handleSubPropertySet(JsonDocument &doc)
{
    const char *msgId  = doc["id"] | "";
    JsonObject params  = doc["params"].as<JsonObject>();
    if (params.isNull()) return;
    mqttRxCount++;   /* 下行计数 */

    const char *pk   = params["productID"]  | "";
    const char *dn   = params["deviceName"] | "";
    JsonObject inner = params["params"].as<JsonObject>();
    if (inner.isNull()) return;

    /* 按 deviceName 匹配节点 */
    int slot = -1;
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (strcmp(nodes[i].deviceName, dn) == 0) { slot = i; break; }
    }
    if (slot < 0)
    {
        DBG_PRINTF("[MQTT] 下发设置: 未知设备 %s/%s\n", pk, dn);
        onenet_replySet(msgId, 404, "device not found");
        return;
    }

    for (JsonPair kv : inner)
    {
        String key      = kv.key().c_str();
        JsonVariant val = kv.value();

        int v;
        if      (val.is<bool>())         v = val.as<bool>() ? 1 : 0;
        else if (val.is<int>())          v = val.as<int>();
        else if (val.is<const char*>())  v = atoi(val.as<const char *>());
        else                             v = 0;

        if (key == SUB_PROP_LED_ENABLE)
        {
            nodes[slot].ledEnable = (v != 0);
            lora_sendControl(nodes[slot].nodeId, "LedEnable", v);
            DBG_PRINTF("[MQTT] 节点%d LED使能=%d\n", nodes[slot].nodeId, v);
        }
        /* ⭐ 僵尸车判定阈值: 按节点设置, 标记待下发 + 保存到 Flash */
        else if (key == SUB_PROP_ZOMBIE_THRESHOLD)
        {
            if (v >= 5 && v <= 2592000) {
                nodes[slot].thresholdNeedsUpdate = true;
                nodes[slot].thresholdValue = v;
                nodes[slot].thresholdRetryCount = 0;  /* 重置重试计数, 新阈值从头开始 */
                saveCertsToLittleFS();  /* ⭐ 立即保存到 Flash, 断电不丢 */
                DBG_PRINTF("[MQTT] 节点%d 僵尸车阈值=%d秒 (已保存Flash, 待PONG下发)\n", nodes[slot].nodeId, v);
            } else {
                DBG_PRINTF("[MQTT] 僵尸车阈值超出范围(5-2592000): %d\n", v);
            }
        }
    }
    onenet_replySet(msgId, 200, "success");
    dataChanged = true;
}

/* 回复平台"子设备服务调用"结果 (thing/sub/service/invoke_reply)
 * 官方响应体: {"id":"..","code":..,"msg":"..","data":{"deviceName":"..",
 *  "productID":"..","identifier":"..","output":{"Result":..,"ActualValue":..}}} */
static void replySubServiceInvoke(const char *msgId, const char *pk, const char *dn,
                                  const char *identifier, int code, const char *msg,
                                  int result, int actualValue)
{
    if (!mqtt.connected()) return;
    StaticJsonDocument<512> doc;
    doc["id"]   = msgId;
    doc["code"] = code;
    doc["msg"]  = msg;
    JsonObject data = doc.createNestedObject("data");
    data["deviceName"] = dn;
    data["productID"]  = pk;
    data["identifier"] = identifier;
    JsonObject output = data.createNestedObject("output");
    output["Result"]      = result;
    output["ActualValue"] = actualValue;
    String out;
    serializeJson(doc, out);
    mqtt.publish(TOPIC_SUB_SERVICE_INVOKE_REPLY, out.c_str());
    mqttTxCount++;   /* 上行计数 */
    DBG_PRINTF("[MQTT] 回复服务调用: %s\n", out.c_str());
}

/* 处理平台下行: 子设备服务调用 (thing/sub/service/invoke).
 * 平台把节点物模型服务调用(SetZombieThreshold, 定义在节点产品上)转发到网关,
 * 消息体: {"id":"..","version":"1.0","params":{"deviceName":"park1",
 *  "productID":"04..","identifier":"SetZombieThreshold","input":{"ThresholdValue":3600}}}
 * 网关解析后定位节点 → 标记阈值下发(LoRa), 收到 ACK 后补回 invoke_reply;
 * 网关单线程不可阻塞, 用 s_pendingServiceReply 跨主循环补回复 */
static void handleSubServiceInvoke(JsonDocument &doc)
{
    const char *msgId = doc["id"] | "";
    JsonObject params = doc["params"].as<JsonObject>();
    if (params.isNull()) return;
    mqttRxCount++;   /* 下行计数 */

    const char *pk         = params["productID"]  | "";
    const char *dn         = params["deviceName"] | "";
    const char *identifier = params["identifier"] | "";
    JsonObject input = params["input"].as<JsonObject>();
    if (input.isNull())
    {
        replySubServiceInvoke(msgId, pk, dn, identifier, 400, "invalid input", 0, 0);
        return;
    }

    /* 只处理僵尸车阈值服务 */
    if (strcmp(identifier, SUB_SERVICE_ZOMBIE_THRESHOLD) != 0)
    {
        DBG_PRINTF("[MQTT] 服务调用: 不支持的 identifier=%s\n", identifier);
        replySubServiceInvoke(msgId, pk, dn, identifier, 404, "unsupported service", 0, 0);
        return;
    }

    int threshold = input["ThresholdValue"] | 0;
    if (threshold < 5 || threshold > 2592000)
    {
        DBG_PRINTF("[MQTT] 服务调用阈值越界: %d\n", threshold);
        replySubServiceInvoke(msgId, pk, dn, identifier, 400, "ThresholdValue out of range", 0, 0);
        return;
    }

    /* 上一次服务调用尚未结束(等 LoRa ACK), 拒绝并提示稍后重试 */
    if (s_pendingServiceReply.active)
    {
        DBG_PRINTF("[MQTT] 服务调用繁忙, 拒绝新调用\n");
        replySubServiceInvoke(msgId, pk, dn, identifier, 200, "busy", 0, 0);
        return;
    }

    /* 按 deviceName 匹配节点 */
    int slot = -1;
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (strcmp(nodes[i].deviceName, dn) == 0) { slot = i; break; }
    }
    if (slot < 0)
    {
        DBG_PRINTF("[MQTT] 服务调用: 未知节点 %s/%s\n", pk, dn);
        replySubServiceInvoke(msgId, pk, dn, identifier, 404, "device not found", 0, 0);
        return;
    }

    /* 节点离线: 立即回失败, 不进入下发流程 */
    if (!nodes[slot].online)
    {
        DBG_PRINTF("[MQTT] 服务调用: 节点%d 离线, 立即回失败\n", nodes[slot].nodeId);
        replySubServiceInvoke(msgId, pk, dn, identifier, 200, "node offline", 0, 0);
        return;
    }

    /* 在线: 写阈值 + 标记待下发(LoRa) + 记录待回复状态 */
    nodes[slot].thresholdValue = threshold;
    nodes[slot].thresholdRetryCount = 0;
    nodes[slot].thresholdNeedsUpdate = true;
    saveCertsToLittleFS();   /* 立即存 Flash, 断电不丢 */

    s_pendingServiceReply.active = true;
    snprintf(s_pendingServiceReply.msgId, sizeof(s_pendingServiceReply.msgId),
             "%s", msgId);
    s_pendingServiceReply.slot        = (uint8_t)slot;
    s_pendingServiceReply.targetValue = (uint32_t)threshold;
    s_pendingServiceReply.deadlineMs  = millis() + SUB_SERVICE_DEADLINE_MS;

    DBG_PRINTF("[MQTT] 服务调用 SetZombieThreshold 节点%d 阈值=%d秒 (待LoRa下发ACK)\n",
               nodes[slot].nodeId, threshold);
}

/* 上报网关自身属性 (property/post): OtaAllow 门控 + OtaProgress 实时进度.
 * 网关自身属性主题为 $sys/{pid}/{gw}/thing/property/post, params 为属性对象.
 * 两者合并成一条上报, 任一变化(或刚重连上线)时由 onenet_loop 触发 */
static void propPostGatewayState(void)
{
    if (!mqtt.connected()) return;
    StaticJsonDocument<384> doc;
    doc["id"] = String(millis());
    doc["version"] = "1.0";
    JsonObject params = doc.createNestedObject("params");
    /* 平台属性上报要求 value 包裹格式: "OtaAllow":{"value":false} */
    JsonObject allow = params.createNestedObject(SUB_PROP_OTA_ALLOW);
    allow["value"] = ota_allow_get();
    JsonObject progress = params.createNestedObject(SUB_PROP_OTA_PROGRESS);
    progress["value"] = ota_progress_get();
    String out;
    serializeJson(doc, out);
    DBG_PRINTF("[MQTT] 上报网关属性: %s\n", out.c_str());
    mqtt.publish(TOPIC_PROP_POST, out.c_str());
    mqttTxCount++;   /* 上行计数 */
}

/* 回复平台"网关自身属性设置"执行结果 (property/set_reply) */
static void replyPropSet(const char *id, int code, const char *msg)
{
    if (!mqtt.connected()) return;
    StaticJsonDocument<256> doc;
    doc["id"]   = id;
    doc["code"] = code;
    doc["msg"]  = msg;
    String output;
    serializeJson(doc, output);
    mqtt.publish(TOPIC_PROP_SET_REPLY, output.c_str());
    DBG_PRINTF("[MQTT] 回复网关属性设置: %s\n", output.c_str());
}

/* 处理平台下行: 网关自身属性设置 (thing/property/set).
 * 目前只支持 OtaAllow 门控; 处理完回 set_reply 给平台 */
static void handleGatewayPropertySet(JsonDocument &doc)
{
    const char *msgId = doc["id"] | "";
    JsonObject params = doc["params"].as<JsonObject>();
    if (params.isNull())
    {
        replyPropSet(msgId, 401, "invalid params");
        return;
    }
    mqttRxCount++;   /* 下行计数 */

    int handled = 0;
    for (JsonPair kv : params)
    {
        String key = kv.key().c_str();
        if (key == SUB_PROP_OTA_ALLOW)
        {
            bool allow = kv.value().as<bool>();
            ota_allow_set(allow);
            handled = 1;
            DBG_PRINTF("[MQTT] 网关 OtaAllow=%s\n", allow ? "true" : "false");
        }
    }
    replyPropSet(msgId, handled ? 200 : 401,
                 handled ? "success" : "unsupported property");
}

static void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
    char buf[768];
    if (length >= sizeof(buf)) length = sizeof(buf) - 1;
    memcpy(buf, payload, length);
    buf[length] = '\0';
    DBG_PRINTF("[MQTT] 收到 topic=%s\n%s\n", topic, buf);

    StaticJsonDocument<768> doc;
    if (deserializeJson(doc, buf))
    {
        DBG_PRINTLN("[MQTT] JSON 解析失败");
        return;
    }

    /* --- 1. 子设备上线回复 --- */
    if (strstr(topic, "thing/sub/login/reply"))
    {
        int code = doc["code"] | -1;
        if (awaitingLogin && loginSlot < LORA_MAX_NODES)
        {
            NodeData &nd = nodes[loginSlot];
            awaitingLogin = false;
            if (code == 200)
            {
                nd.subLogin      = true;
                nd.logoutPending = false;
                DBG_PRINTF("[MQTT] 子设备%d 上线成功\n", nd.nodeId);
                subPost(loginSlot);            /* 上线成功立即上报一次 */
            }
            else
            {
                nd.loginPending = true;        /* 失败, 稍后重试 */
                DBG_PRINTF("[MQTT] 子设备%d 上线失败 code=%d\n",
                           nd.nodeId, code);
            }
        }
        return;
    }

    /* --- 2. 批量上报回复 (只打错误) --- */
    if (strstr(topic, "thing/pack/post/reply"))
    {
        int code = doc["code"] | -1;
        if (code != 200)
            DBG_PRINTF("[MQTT] 批量上报回复 code=%d\n", code);
        return;
    }

    /* --- 3. 子设备属性设置 (下行控制) --- */
    if (strstr(topic, "thing/sub/property/set"))
    {
        DBG_PRINTF("[MQTT] 收到子设备属性设置: %s\n", topic);
        handleSubPropertySet(doc);
        return;
    }

    /* --- 3.5 子设备服务调用 (thing/sub/service/invoke) --- */
    if (strstr(topic, "thing/sub/service/invoke"))
    {
        DBG_PRINTF("[MQTT] 收到子设备服务调用: %s\n", topic);
        handleSubServiceInvoke(doc);
        return;
    }

    /* --- 4. 网关自身属性设置 (下行控制): OtaAllow 门控 --- */
    if (strstr(topic, "thing/property/set"))
    {
        handleGatewayPropertySet(doc);
        return;
    }

    /* --- 5. 网关自身属性上报回执 (property/post/reply): 排查平台是否拒收 --- */
    if (strstr(topic, "thing/property/post/reply"))
    {
        int code = doc["code"] | -1;
        DBG_PRINTF("[MQTT] 网关属性上报回执 code=%d\n", code);
        return;
    }
}

/* ==================== 公开函数 ==================== */

void onenet_init(void)
{
    mqtt.setServer(ONENET_SERVER, ONENET_PORT);
    mqtt.setCallback(mqtt_callback);
    mqtt.setBufferSize(2048);
    mqtt.setKeepAlive(60);
    DBG_PRINTLN("[MQTT] OneNET 网关客户端已初始化");
}

bool onenet_connect(void)
{
    if (mqtt.connected()) return true;
    DBG_PRINTLN("[MQTT] 正在连接 OneNET (网关)...");
    bool ok = mqtt.connect(ONENET_DEVID, ONENET_PROID, ONENET_TOKEN);
    if (!ok)
    {
        /* CONNACK 错误码处理 (借鉴参考项目 wifi.c) */
        switch (mqtt.state())
        {
            case -4: DBG_PRINTLN("[MQTT] 连接超时, 稍后重试");              break;
            case -3: DBG_PRINTLN("[MQTT] 连接丢失, 将重试");                break;
            case -2: DBG_PRINTLN("[MQTT] 连接失败 (TCP), 重试中");          break;
            case -1: DBG_PRINTLN("[MQTT] 已断开, 将重试");                  break;
            case 1:  DBG_PRINTLN("[MQTT] 拒绝: 不支持的协议版本");          break;
            case 2:  DBG_PRINTLN("[MQTT] 拒绝: 客户端标识无效");            break;
            case 3:  DBG_PRINTLN("[MQTT] 拒绝: 服务器不可用");              break;
            case 4:  DBG_PRINTLN("[MQTT] 拒绝: 用户名/密码错误 (检查Token?)"); break;
            case 5:  DBG_PRINTLN("[MQTT] 拒绝: 未授权 (检查Token)");        break;
            default: DBG_PRINTF("[MQTT] 连接失败, state=%d\n", mqtt.state()); break;
        }
        return false;
    }
    mqtt.subscribe(TOPIC_SUB_LOGIN_REPLY);
    mqtt.subscribe(TOPIC_PACK_POST_REPLY);
    mqtt.subscribe(TOPIC_SUB_SET);
    mqtt.subscribe(TOPIC_SUB_SERVICE_INVOKE);   /* ⭐ 子设备服务调用下行 */
    mqtt.subscribe(TOPIC_PROP_SET);          /* 网关自身属性下行: OtaAllow 门控 */
    mqtt.subscribe(TOPIC_PROP_POST_REPLY);   /* 网关自身属性上报回执(排查) */
    DBG_PRINTLN("[MQTT] 连接成功, 已订阅子设备登录/上报/设置/服务调用 + 网关属性主题");
    sysEventFlag |= SYS_EVENT_MQTT_CONNECTED;

    /* MQTT 重连后平台会话重置, 已注册节点全部需要重新代上线;
     * 同时统一先代下线一遍: 清掉平台可能残留的"子设备在线"标记,
     * 防止网关重启/断线期间出现过期伪在线 (离线节点后续不会被上线) */
    awaitingLogin = false;
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].certSent)
        {
            nodes[i].subLogin       = false;
            nodes[i].loginPending   = true;
            nodes[i].logoutPending  = true;   /* 统一先下架, 等 PING 通再上线 */
        }
    }
    return true;
}

void onenet_disconnect(void)
{
    mqtt.disconnect();
    sysEventFlag &= ~SYS_EVENT_MQTT_CONNECTED;
}

static bool s_lastOtaAllow     = false;
static int  s_lastOtaProgress  = -1;
static bool s_otaPropReported  = false;

void onenet_loop(void)
{
    mqtt.loop();

    /* 同步标志位: 如果 mqtt 掉线了, 清除连接标志 (统一走收口函数).
     * 底层断线(TCP RST/keepalive 超时)由 PubSubClient 检测并断开 */
    if (!mqtt.connected() && (sysEventFlag & SYS_EVENT_MQTT_CONNECTED))
    {
        onenet_disconnect();
        s_pendingServiceReply.active = false;   /* 断线无法回 invoke_reply, 丢弃待回复状态 */
    }

    /* ⭐ 同步服务调用截止检查: 超过 9s 未收到 LoRa ACK 强制回失败
     * (平台同步调用超时约10s, 必须赶在前面回复, 否则平台判定超时) */
    if (s_pendingServiceReply.active &&
        (long)(millis() - s_pendingServiceReply.deadlineMs) > 0)
    {
        NodeData &nd = nodes[s_pendingServiceReply.slot];
        DBG_PRINTF("[MQTT] 服务调用超时(%dms), 回失败\n", (int)SUB_SERVICE_DEADLINE_MS);
        replySubServiceInvoke(s_pendingServiceReply.msgId,
                              nd.productKey, nd.deviceName,
                              SUB_SERVICE_ZOMBIE_THRESHOLD,
                              200, "timeout", 0, 0);
        s_pendingServiceReply.active = false;
    }

    /* 网关自身属性上报: OtaAllow / OtaProgress 任一变化,
     * 或刚(重)连上线后首次, 合并成一条上报当前值, 供 App 实时查询 */
    if (mqtt.connected())
    {
        bool allow = ota_allow_get();
        int  pg    = ota_progress_get();
        if (!s_otaPropReported ||
            allow != s_lastOtaAllow ||
            pg != s_lastOtaProgress)
        {
            propPostGatewayState();
            s_lastOtaAllow    = allow;
            s_lastOtaProgress = pg;
            s_otaPropReported = true;
        }
    }
    else
    {
        s_otaPropReported = false;   /* 掉线重置, 重连后再报一次 */
    }
}

bool onenet_connected(void) { return mqtt.connected(); }

/* 周期调用: 处理代下线/代上线/批量上报
 * 注意: 上线/下线一次只发一条, 避免回复无法对应身份 */
void onenet_uploadAll(void)
{
    if (!mqtt.connected()) return;
    uint32_t now = millis();
    uint8_t  i;

    /* 1. 处理待代下线 */
    for (i = 0; i < nodeCount; i++)
    {
        if (nodes[i].logoutPending)
        {
            subLogout(i);
            return;
        }
    }

    /* 2. 正在等上线回复: 超时则重新排队 */
    if (awaitingLogin)
    {
        if (now - loginSentAt > SUB_LOGIN_TIMEOUT_MS)
        {
            DBG_PRINTLN("[MQTT] 子设备上线超时, 将重试");
            awaitingLogin = false;
            if (loginSlot < LORA_MAX_NODES)
                nodes[loginSlot].loginPending = true;
        }
        return;
    }

    /* 3. 处理待代上线 (一次一条; 仅"确认存活"的节点才上线,
     *    离线节点不上线, 平台在线列表始终与真实状态一致) */
    for (i = 0; i < nodeCount; i++)
    {
        if (nodes[i].loginPending && nodes[i].online)
        {
            subLogin(i);
            return;
        }
    }

    /* 4. 已上线节点批量上报: 一轮内所有在线节点合并为一条 pack/post
     *    (满足平台"上行≤1次/s"限制, 避免 N 台节点瞬时 N 条上行超限) */
    subPostBatch();
}

/* 回复平台"子设备属性设置"执行结果 */
void onenet_replySet(const char *id, int code, const char *msg)
{
    if (!mqtt.connected()) return;
    StaticJsonDocument<256> doc;
    doc["id"]   = id;
    doc["code"] = code;
    doc["msg"]  = msg;
    String output;
    serializeJson(doc, output);
    mqtt.publish(TOPIC_SUB_SET_REPLY, output.c_str());
    DBG_PRINTF("[MQTT] 回复平台: %s\n", output.c_str());
}

/* ⭐ 供 lora_handler 调用: LoRa 阈值下发结果确认后, 补回"同步服务调用"回复.
 * 仅当存在待回复的服务调用且节点匹配时才回; 属性路径下发(控制台改属性)
 * 不置 pending, 调用会被忽略, 不会产生多余回复.
 * success=true → Result=1/ActualValue=实际生效值; false → Result=0/ActualValue=0 */
void onenet_notifyServiceResult(uint8_t slot, bool success, uint32_t value)
{
    if (!s_pendingServiceReply.active || s_pendingServiceReply.slot != slot)
        return;   /* 非服务调用触发(或已回复/已超时), 忽略 */

    NodeData &nd = nodes[slot];
    replySubServiceInvoke(s_pendingServiceReply.msgId,
                          nd.productKey, nd.deviceName,
                          SUB_SERVICE_ZOMBIE_THRESHOLD,
                          200, success ? "success" : "failed",
                          success ? 1 : 0,
                          success ? (int)value : 0);
    s_pendingServiceReply.active = false;
    DBG_PRINTF("[MQTT] 服务调用结果已回复平台 (节点%d, %s)\n",
               nodes[slot].nodeId, success ? "成功" : "失败");
}
