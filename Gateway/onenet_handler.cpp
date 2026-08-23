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
    }
    onenet_replySet(msgId, 200, "success");
    dataChanged = true;
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
        handleSubPropertySet(doc);
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
    DBG_PRINTLN("[MQTT] 连接成功, 已订阅子设备登录/上报/设置主题");
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

void onenet_loop(void)
{
    mqtt.loop();

    /* 同步标志位: 如果 mqtt 掉线了, 清除连接标志 (统一走收口函数).
     * 底层断线(TCP RST/keepalive 超时)由 PubSubClient 检测并断开 */
    if (!mqtt.connected() && (sysEventFlag & SYS_EVENT_MQTT_CONNECTED))
    {
        onenet_disconnect();
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
