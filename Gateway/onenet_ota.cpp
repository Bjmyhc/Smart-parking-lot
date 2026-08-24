/* onenet_ota.cpp - OneNET Studio 固件升级 (fuse-ota HTTP API) 客户端实现
 *
 * 与平台"固件升级"服务交互, 实现平台自动下发 OTA:
 *   1. 上报固件版本 (FOTA=网关 / SOTA=节点)
 *   2. 周期检测升级任务 (check)
 *   3. 下载固件到 LittleFS
 *   4. 上报升级进度 (status)
 *   5. 分发: FOTA→网关自升级(Updater); SOTA→现有 LoRa OTA 链路转发节点
 *
 * 鉴权(OneNET Studio API, version=2022-05-01):
 *   StringForSignature = et + "\n" + method + "\n" + res + "\n" + version
 *   sign = base64( hmac_sha1( base64_decode(accessKey), StringForSignature ) )
 *   Authorization = "version=2022-05-01&res={urlenc(res)}&et={expire}"
 *                   "&method=sha1&sign={urlenc(sign)}"
 * 双身份双凭据:
 *   SOTA(节点固件,type=2): res = userid/{用户ID} (用户级 accessKey)
 *                           URL 用节点产品/设备 04kjwU9TC7/Park001
 *   FOTA(网关固件,type=1): res = products/{网关产品}/devices/PGW001 (设备级)
 *
 * 注: 断点续传(Range)为后续增强, 当前整包下载失败自动重下.
 */
#include "onenet_ota.h"
#include "platform_cfg.h"
#include "app_cfg.h"
#include "ota_handler.h"
#include "lora_protocol.h"      /* OTA_FW_FILE / OTA_FW_MAGIC */
#include "node_data.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <coredecls.h>      /* configTime (ESP8266) */
#include <time.h>

#if defined(ESP8266)
  #include <Updater.h>          /* 网关自升级 */
#endif

/* ==================== 常量 ==================== */
#define OTA_API_VERSION         "2022-05-01"
#define OTA_AUTH_METHOD         "sha1"
#define OTA_CHECK_INTERVAL_MS   30000   /* 周期检测升级任务 */
#define OTA_REPORT_EVERY        10      /* 每N次检测才上报一次版本(降频省HTTP, 减少阻塞) */
#define OTA_PROGRESS_REPORT_MS  2000    /* LoRa 分发进度上报最小间隔(ms) */
#define OTA_PROGRESS_MIN_DELTA  5       /* 进度变化≥此值才上报(减少HTTP请求/阻塞) */
#define OTA_DL_CHUNK            256     /* 下载流式写入块大小 */
#define OTA_GW_FILE             "/gw_firmware.bin"   /* 网关固件临时文件 */
#define OTA_FW_HDR_MAGIC        OTA_FW_MAGIC         /* 节点固件魔数 0xA55A */
#define OTA_STATE_RESET_MS      10000   /* 完成/失败态保持时间: 让 App 轮询看到终端状态,
                                         * 之后再复位 OtaProgress 供下一次升级 */

/* 任务类型 (fuse-ota check 的 type 参数) */
#define OTA_TYPE_FOTA           1       /* 模组/网关固件 */
#define OTA_TYPE_SOTA           2       /* MCU/节点固件 */

/* 节点固件文件头 (12B, 与 Tool/fw_pack.py 一致) */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t version;
    uint32_t length;
    uint32_t crc32;
} FwHeader_t;

/* ==================== OTA 身份 ====================
 * SOTA(节点固件): 用户级 accessKey, res=userid/{用户ID}, URL 指向节点产品
 * FOTA(网关固件): 网关设备级 accessKey, res=products/.../devices/PGW001 */
typedef struct {
    const char *res;    /* 参与签名的资源路径 */
    const char *key;    /* accessKey (Base64) */
    const char *proid;  /* URL 用产品ID */
    const char *devid;  /* URL 用设备名 */
} OtaIdentity_t;

static const OtaIdentity_t OTA_ID_SOTA = {
    "userid/" ONENET_USER_ID,
    ONENET_USER_ACCESS_KEY,
    NODE_PROID,
    NODE_DEVID,
};
static const OtaIdentity_t OTA_ID_FOTA = {
    "products/" ONENET_PROID "/devices/" ONENET_DEVID,
    ONENET_OTA_ACCESS_KEY,
    ONENET_PROID,
    ONENET_DEVID,
};

/* ==================== 状态机 ====================
 * 每个状态只执行一次 HTTP 请求(≤3s)即返回, 避免长时间阻塞 loop,
 * 影响 LoRa 轮询导致节点失联. 完整周期: 上报→检FOTA→检SOTA→下载→分发 */
typedef enum {
    OTA_PLAT_IDLE,          /* 空闲: 等下次检测周期 */
    OTA_PLAT_REPORT_SOTA,   /* 上报节点版本 (用户级身份) */
    OTA_PLAT_REPORT_FOTA,   /* 上报网关版本 (设备级身份) */
    OTA_PLAT_CHECK_FOTA,    /* 检测 FOTA (网关) 任务 */
    OTA_PLAT_CHECK_SOTA,    /* 检测 SOTA (节点) 任务 */
    OTA_PLAT_DOWNLOAD,      /* 下载固件 */
    OTA_PLAT_DISPATCH,      /* 分发: 自升级 / LoRa 转发 */
    OTA_PLAT_FINISH,        /* 上报完成/失败 */
    OTA_PLAT_RESET          /* 保持终端状态片刻后复位 OTA 属性, 再回空闲 */
} OtaPlatState_t;

/* ==================== 静态变量 ==================== */
static OtaPlatState_t s_st   = OTA_PLAT_IDLE;
static uint32_t s_lastCheck  = 0;
static uint8_t  s_checkCnt   = 0;    /* 版本上报降频计数 */
static bool     s_busy       = false;

/* OTA 全网一键升级门控:
 *   s_otaAllow   : App 经物模型下发 OtaAllow=1 的确认标志
 *   s_pendingSota: 已检测到 SOTA 任务但尚未获 App 确认 (挂起等待) */
static bool     s_otaAllow    = false;
static bool     s_pendingSota = false;

/* OTA 实时进度 (网关自身物模型属性 OtaProgress, MQTT 上报给 App 驱动精确进度条):
 * 阶段状态已改用官方 fuse-ota $tid/check 接口, 网关不再维护/上报 OtaStatus */
static int      s_otaProgress = 0;

/* 更新 OTA 进度并打日志; onenet_handler 每轮比对变化后经 MQTT 上报 */
static void ota_progressSet(int progress)
{
    s_otaProgress = progress;
    DBG_PRINTF("[OTA][状态] progress=%d\n", progress);
}

int ota_progress_get(void) { return s_otaProgress; }

/* 终端状态(完成/失败)进入 OTA_PLAT_RESET 的时间戳, 用于延迟复位属性 */
static uint32_t s_stateResetAt = 0;

/* LoRa 分发进度上报状态 (降频上报 step 给平台进度栏) */
static uint32_t s_lastProgressReport = 0;
static int      s_lastProgressStep   = -1;

/* 当前任务信息 (check 结果) */
static long     s_tid        = 0;
static long     s_taskSize   = 0;
static long     s_taskType   = 0;
static String   s_taskVer;

/* ==================== 密码学工具 (自实现, 无外部依赖) ==================== */

/* --- SHA1 (RFC 3174) --- */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
} Sha1Ctx;

static void sha1_init(Sha1Ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->len = 0;
}

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1Ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8)  |  (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2],
             d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);     k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;              k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;              k = 0xCA62C1D6; }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc;
    c->h[3] += d; c->h[4] += e;
}

static void sha1_update(Sha1Ctx *c, const uint8_t *data, size_t len)
{
    c->len += len;
    /* 先处理 buf 中残留 */
    size_t idx = (size_t)(c->len - len) & 63;
    while (len > 0)
    {
        size_t take = 64 - idx;
        if (take > len) take = len;
        memcpy(c->buf + idx, data, take);
        idx += take; data += take; len -= take;
        if (idx == 64) { sha1_block(c, c->buf); idx = 0; }
    }
}

static void sha1_final(Sha1Ctx *c, uint8_t out[20])
{
    uint64_t bitlen = c->len << 3;
    uint8_t pad = 0x80;
    size_t used = (size_t)c->len & 63;
    size_t need = (used < 56) ? (56 - used) : (120 - used);
    static const uint8_t zeros[64] = {0};
    sha1_update(c, &pad, 1);
    if (need > 1) sha1_update(c, zeros, need - 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[7 - i] = (uint8_t)(bitlen >> (i * 8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++)
    {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    Sha1Ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* --- HMAC-SHA1 (RFC 2104) --- */
static void hmac_sha1(const uint8_t *key, size_t keyLen,
                      const uint8_t *msg, size_t msgLen, uint8_t out[20])
{
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (keyLen > 64) sha1(key, keyLen, k);
    else memcpy(k, key, keyLen);

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }

    Sha1Ctx c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, msgLen);
    uint8_t inner[20];
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

/* --- Base64 --- */
static const char s_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *in, size_t inLen, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < inLen; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < inLen) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < inLen) v |= in[i+2];
        out[o++] = s_b64[(v >> 18) & 0x3F];
        out[o++] = s_b64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < inLen) ? s_b64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < inLen) ? s_b64[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

static size_t base64_decode(const char *in, uint8_t *out)
{
    int tbl[256];
    for (int i = 0; i < 256; i++) tbl[i] = -1;
    for (int i = 0; i < 64; i++) tbl[(uint8_t)s_b64[i]] = i;

    size_t o = 0;
    uint32_t v = 0;
    int bits = 0;
    for (; *in; in++)
    {
        if (*in == '=' || *in == '\r' || *in == '\n' || *in == ' ') continue;
        int d = tbl[(uint8_t)*in];
        if (d < 0) break;
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[o++] = (uint8_t)((v >> bits) & 0xFF);
        }
    }
    return o;
}

/* --- URL 编码 (仅编码签名/base64 需要保留的字符) --- */
static void url_encode(const char *in, char *out, size_t outSize)
{
    size_t o = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (const char *p = in; *p && o + 3 < outSize; p++)
    {
        uint8_t ch = (uint8_t)*p;
        bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.' || ch == '~';
        if (unreserved)
            out[o++] = (char)ch;
        else
        {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 0xF];
        }
    }
    out[o] = '\0';
}

/* ==================== 鉴权 ==================== */

/* 生成 Authorization 头 (按身份选择 res/key) */
static String makeAuthorization(const OtaIdentity_t *id)
{
    /* 过期时间: NTP 同步后取当前+300s (平台规范), 未同步用 2100 年兜底避免立即过期 */
    time_t now = time(nullptr);
    long et;
    if (now < 1600000000L)
        et = 4102444800L + 300L;      /* 2100-01-01 + 5分钟 */
    else
        et = (long)now + 300L;

    /* OneNET 签名规范: StringForSignature = et + "\n" + method + "\n" + res + "\n" + version
     * 注意 res 参与签名用原始形式 (userid/xxx 或 products/xxx/devices/xxx),
     * URL 编码只用于 Authorization 头 */
    char strToSign[256];
    snprintf(strToSign, sizeof(strToSign), "%ld\n%s\n%s\n%s",
             et, OTA_AUTH_METHOD, id->res, OTA_API_VERSION);

    /* 解码 accessKey (Base64) → HMAC-SHA1 → Base64 → URL 编码 */
    uint8_t key[64];
    size_t keyLen = base64_decode(id->key, key);
    uint8_t digest[20];
    hmac_sha1(key, keyLen, (const uint8_t *)strToSign, strlen(strToSign), digest);

    char signRaw[64];
    base64_encode(digest, 20, signRaw);

    char resEnc[128], signEnc[192];
    url_encode(id->res, resEnc, sizeof(resEnc));
    url_encode(signRaw, signEnc, sizeof(signEnc));

    String auth;
    auth.reserve(320);
    auth = "version=";
    auth += OTA_API_VERSION;
    auth += "&res=";
    auth += resEnc;
    auth += "&et=";
    auth += String(et);
    auth += "&method=";
    auth += OTA_AUTH_METHOD;
    auth += "&sign=";
    auth += signEnc;
    return auth;
}

/* ==================== HTTP 工具 ==================== */

/* 发起带鉴权的请求, 返回响应体 (JSON) */
static int otaHttpRequest(const OtaIdentity_t *id, const char *method,
                          const char *path, const char *body, String &resp)
{
    WiFiClient wc;
    HTTPClient http;
    String url = String("http://") + ONENET_OTA_SERVER + path;
    http.begin(wc, url);
    http.setTimeout(1500);          /* 短超时: 避免阻塞 loop 影响 LoRa 轮询 */
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    String auth = makeAuthorization(id);
    http.addHeader("Authorization", auth);
    http.addHeader("Content-Type", "application/json");

    int code;
    if (String(method) == "POST")
        code = http.POST((uint8_t *)body, strlen(body));
    else
        code = http.GET();

    if (code > 0)
        resp = http.getString();
    http.end();
    return code;
}

/* 当前任务对应身份 (SOTA=用户级/节点, FOTA=设备级/网关) */
static const OtaIdentity_t *otaTaskIdentity(void)
{
    return (s_taskType == OTA_TYPE_FOTA) ? &OTA_ID_FOTA : &OTA_ID_SOTA;
}

/* 节点当前固件版本字符串: 优先用在线节点上报的 FwVersion (自动跟随节点升级),
 * 无在线节点/版本为空时回退到编译期宏 NODE_FW_VERSION */
static const char *otaNodeCurVersion(void)
{
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].online && nodes[i].fwVersion[0] != '\0')
            return nodes[i].fwVersion;
    }
    return NODE_FW_VERSION;
}

/* ==================== 平台 API 封装 ==================== */

/* 上报节点版本 (SOTA): 用户级身份, 节点产品下记录 s_version.
 * 注: 平台 /version 要求 s_version 与 f_version 两个字段都填, 缺一报格式错误. */
static bool otaReportVersionSota(void)
{
    String body = String("{\"s_version\":\"") + otaNodeCurVersion() +
                  "\",\"f_version\":\"" + GW_FW_VERSION + "\"}";
    String path = String("/fuse-ota/") + NODE_PROID + "/" + NODE_DEVID + "/version";
    String resp;
    int code = otaHttpRequest(&OTA_ID_SOTA, "POST", path.c_str(), body.c_str(), resp);
    DBG_PRINTF("[OTA][平台] 上报节点版本 %s: HTTP %d %s\n", body.c_str(), code, resp.c_str());
    return (code == 200);
}

/* 上报网关版本 (FOTA): 设备级身份, 网关产品下记录 f_version */
static bool otaReportVersionFota(void)
{
    String body = String("{\"s_version\":\"") + otaNodeCurVersion() +
                  "\",\"f_version\":\"" + GW_FW_VERSION + "\"}";
    String path = String("/fuse-ota/") + ONENET_PROID + "/" + ONENET_DEVID + "/version";
    String resp;
    int code = otaHttpRequest(&OTA_ID_FOTA, "POST", path.c_str(), body.c_str(), resp);
    DBG_PRINTF("[OTA][平台] 上报网关版本 %s: HTTP %d %s\n", body.c_str(), code, resp.c_str());
    return (code == 200);
}

/* 检测任务: check?type={1|2}&version={当前版本}
 * 有任务返回 true 并填充 s_tid/s_taskSize/s_taskVer */
static bool otaCheckTask(const OtaIdentity_t *id, int type, const char *curVersion)
{
    String path = String("/fuse-ota/") + id->proid + "/" + id->devid +
                  "/check?type=" + String(type) + "&version=" + curVersion;
    String resp;
    int code = otaHttpRequest(id, "GET", path.c_str(), nullptr, resp);
    if (code != 200)
    {
        DBG_PRINTF("[OTA][平台] 检测任务失败: HTTP %d\n", code);
        return false;
    }

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, resp))
    {
        DBG_PRINTF("[OTA][平台] 检测任务响应解析失败: %s\n", resp.c_str());
        return false;
    }
    if (doc["code"] | -1)
    {
        DBG_PRINTF("[OTA][平台] 检测任务 code!=0: %s\n", resp.c_str());
        return false;
    }
    JsonObject data = doc["data"];
    if (data.isNull())
        return false;                       /* 无任务 */

    s_tid      = data["tid"] | 0L;
    s_taskSize = data["size"] | 0L;
    s_taskType = type;
    s_taskVer  = data["target"] | "";
    long status = data["status"] | 0L;
    DBG_PRINTF("[OTA][平台] 检测到任务 type=%d tid=%ld target=%s size=%ld status=%ld\n",
               type, s_tid, s_taskVer.c_str(), s_taskSize, status);
    /* status: 1=待升级 2=下载中 3=升级中; 4=完成 5=失败 → 跳过已结束任务 */
    if (status > 3)
        return false;
    return (s_tid != 0);
}

/* 下载固件到指定文件, 返回字节数(-1 失败) */
static long otaDownloadFile(const OtaIdentity_t *id, long tid, const char *filePath)
{
    if (!LittleFS.begin())
    {
        DBG_PRINTLN("[OTA][平台] LittleFS 挂载失败");
        return -1;
    }
    LittleFS.remove(filePath);

    String path = String("/fuse-ota/") + id->proid + "/" + id->devid +
                  "/" + String(tid) + "/download";
    String url = String("http://") + ONENET_OTA_SERVER + path;

    WiFiClient wc;
    HTTPClient http;
    http.begin(wc, url);
    http.setTimeout(1500);          /* 短超时: 避免阻塞 loop 影响 LoRa 轮询 */
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Authorization", makeAuthorization(id));

    DBG_PRINTF("[OTA][平台] 下载固件 tid=%ld -> %s\n", tid, filePath);
    int code = http.GET();
    if (code != HTTP_CODE_OK && code != HTTP_CODE_PARTIAL_CONTENT)
    {
        DBG_PRINTF("[OTA][平台] 下载失败: HTTP %d\n", code);
        http.end();
        return -1;
    }

    File f = LittleFS.open(filePath, "w");
    if (!f)
    {
        DBG_PRINTLN("[OTA][平台] 无法创建固件文件");
        http.end();
        return -1;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[OTA_DL_CHUNK];
    long total = 0, expected = http.getSize();
    int lastPct = -1;
    while (http.connected() && stream->available())
    {
        int n = stream->readBytes(buf, sizeof(buf));
        if (n > 0)
        {
            f.write(buf, n);
            total += n;
            if (expected > 0)
            {
                int pct = (int)(total * 100 / expected);
                if (pct != lastPct && (pct % 25 == 0 || pct == 100))
                {
                    DBG_PRINTF("[OTA][平台] 下载进度: %d%% (%ld/%ld)\n",
                               pct, total, expected);
                    lastPct = pct;
                }
            }
        }
        yield();
    }
    f.close();
    http.end();

    if (total <= 0)
    {
        LittleFS.remove(filePath);
        DBG_PRINTLN("[OTA][平台] 下载为空, 失败");
        return -1;
    }
    DBG_PRINTF("[OTA][平台] 下载完成: %ld 字节\n", total);
    return total;
}

/* 上报升级状态: {"step":进度(0-100), 201=完成} */
static void otaReportStatus(long tid, int step)
{
    const OtaIdentity_t *id = otaTaskIdentity();
    String body = String("{\"step\":") + step + "}";
    String path = String("/fuse-ota/") + id->proid + "/" + id->devid +
                  "/" + String(tid) + "/status";
    String resp;
    int code = otaHttpRequest(id, "POST", path.c_str(), body.c_str(), resp);
    DBG_PRINTF("[OTA][平台] 上报进度 step=%d: HTTP %d\n", step, code);
}

/* 上报 LoRa 链路分发进度 (仅 SOTA; step 1-99).
 * 降频: 间隔≥OTA_PROGRESS_REPORT_MS 且变化≥OTA_PROGRESS_MIN_DELTA 才上报,
 * 减少 HTTP 请求并避免阻塞 loop 影响 LoRa 收包 */
static void otaReportProgress(void)
{
    if (s_taskType != OTA_TYPE_SOTA) return;
    if (ota_getState() == OTA_IDLE) return;   /* 未在分发 */
    if (s_st != OTA_PLAT_FINISH)   return;    /* 非等待 LoRa 链路完成阶段 */

    const OtaProgress_t *p = ota_getProgress();
    if (p->totalBytes <= 0) return;           /* 触发/下载阶段, 无发送进度 */

    uint32_t now = millis();
    if (now - s_lastProgressReport < OTA_PROGRESS_REPORT_MS) return;

    /* 已发送字节 → 1~99 的 step */
    int pct = (int)(((uint32_t)p->sentBytes * 99u) / (uint32_t)p->totalBytes);
    if (pct < 1)  pct = 1;
    if (pct > 99) pct = 99;

    /* 进度变化不足阈值则不重复上报 */
    if (pct < s_lastProgressStep)
        s_lastProgressStep = 0;   /* 新一轮分发开始(pct回退): 复位降频状态 */
    if (pct - s_lastProgressStep < OTA_PROGRESS_MIN_DELTA) return;
    s_lastProgressStep   = pct;
    s_lastProgressReport = now;
    s_otaProgress = pct;   /* 同步真实分发进度到网关物模型属性, 供 App 读取 */

    const OtaIdentity_t *id = otaTaskIdentity();
    String body = String("{\"step\":") + pct + "}";
    String path = String("/fuse-ota/") + id->proid + "/" + id->devid +
                  "/" + String(s_tid) + "/status";
    String resp;
    int code = otaHttpRequest(id, "POST", path.c_str(), body.c_str(), resp);
    DBG_PRINTF("[OTA][平台] 上报进度 step=%d: HTTP %d\n", pct, code);
}

/* ==================== 分发 ==================== */

/* FOTA: 网关自升级 (Updater 从文件写入 Flash, 成功后重启) */
static void otaApplyGatewayFirmware(void)
{
    File f = LittleFS.open(OTA_GW_FILE, "r");
    if (!f)
    {
        DBG_PRINTLN("[OTA][平台] 网关固件文件打开失败");
        return;
    }
    DBG_PRINTF("[OTA][平台] 网关固件 %d 字节, 写入 Flash...\n", f.size());

    if (!Update.begin(f.size(), U_FLASH))
    {
        DBG_PRINTLN("[OTA][平台] Update.begin 失败");
        f.close();
        return;
    }
    size_t written = Update.writeStream(f);
    f.close();
    if (written != f.size())
    {
        DBG_PRINTF("[OTA][平台] 写入不完整 %d/%d, 放弃\n",
                   (int)written, (int)f.size());
        Update.end();               /* 失败结束, 不触发重启 */
        return;
    }
    if (!Update.end())
    {
        DBG_PRINTF("[OTA][平台] Update.end 失败: %s\n", Update.getErrorString());
        return;
    }
    DBG_PRINTLN("[OTA][平台] 网关固件写入成功, 即将重启!");
    ota_progressSet(100);   /* 网关自升级: 写入完成 */
    otaReportStatus(s_tid, 201);
    delay(500);
    ESP.restart();
}

/* 校验节点固件文件头 (魔数/长度), 通过则交给 LoRa OTA 链路 */
static bool otaValidateNodeFirmware(void)
{
    File f = LittleFS.open(OTA_FW_FILE, "r");
    if (!f) return false;
    FwHeader_t hdr;
    bool ok = false;
    if (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr))
    {
        uint32_t fwLen = f.size() - sizeof(hdr);
        DBG_PRINTF("[OTA][平台] 节点固件头: magic=0x%04X ver=0x%04X len=%u file=%u\n",
                   hdr.magic, hdr.version, (unsigned)hdr.length, (unsigned)fwLen);
        if (hdr.magic == OTA_FW_HDR_MAGIC && hdr.length == fwLen)
            ok = true;
    }
    f.close();
    return ok;
}

/* SOTA 目标节点: 第一个在线节点 */
static uint8_t otaTargetNode(void)
{
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].online)
        {
            DBG_PRINTF("[OTA][平台] 目标节点=%d (%s)\n",
                       nodes[i].nodeId, nodes[i].deviceName);
            return nodes[i].nodeId;
        }
    }
    DBG_PRINTLN("[OTA][平台] 无在线节点, 暂不升级");
    return 0;
}

/* ==================== 公开函数 ==================== */

/* App 下发 OtaAllow: 全网一键升级确认门控.
 * 若当前正挂起等待确认(已检测到任务), 立即转入下载执行 */
void ota_allow_set(bool allow)
{
    s_otaAllow = allow;
    DBG_PRINTF("[OTA][门控] OtaAllow=%s (pending=%d tid=%ld st=%d)\n",
               allow ? "true" : "false", s_pendingSota, s_tid, (int)s_st);
    if (allow && s_pendingSota && s_tid != 0 && s_st == OTA_PLAT_IDLE)
    {
        s_pendingSota = false;
        s_st = OTA_PLAT_DOWNLOAD;
        DBG_PRINTLN("[OTA][门控] 已获App确认, 立即执行升级");
    }
}

bool ota_allow_get(void) { return s_otaAllow; }

/* 升级结束(完成/失败): 复位门控, 一次确认只对一次任务生效 */
static void ota_gateReset(void)
{
    s_otaAllow    = false;
    s_pendingSota = false;
}

void onenet_ota_init(void)
{
    s_st = OTA_PLAT_IDLE;
    s_lastCheck = 0;
    s_busy = false;
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");   /* 签名用 */
    DBG_PRINTLN("[OTA][平台] OneNET OTA 客户端初始化完成");
}

bool onenet_ota_busy(void)
{
    return s_busy;
}

void onenet_ota_tick(void)
{
    /* 需要网络 */
    if (WiFi.status() != WL_CONNECTED) return;

    /* 平台 LoRa OTA 处理器忙时, 不启动新流程 */
    if (ota_getState() != OTA_IDLE)
    {
        s_busy = false;
        /* LoRa 分发进行中: 周期上报进度到平台进度栏 */
        otaReportProgress();
        return;
    }

    switch (s_st)
    {
    case OTA_PLAT_IDLE:
        s_busy = false;
        if (millis() - s_lastCheck >= OTA_CHECK_INTERVAL_MS)
        {
            s_lastCheck = millis();
            /* 版本上报降频: 平台需要先记录当前版本才能检测任务, 但每次
             * 检测都上报太费 HTTP; 每 OTA_REPORT_EVERY 次才上报一轮,
             * 平时直接 check, 降低对 loop 的阻塞(保护 LoRa 轮询) */
            if (++s_checkCnt >= OTA_REPORT_EVERY)
            {
                s_checkCnt = 0;
                s_st = OTA_PLAT_REPORT_SOTA;
            }
            else
                s_st = OTA_PLAT_CHECK_FOTA;
            s_busy = true;
        }
        break;

    case OTA_PLAT_REPORT_SOTA:
        /* 1. 上报节点版本 (用户级身份, 节点产品下) */
        otaReportVersionSota();
        s_st = OTA_PLAT_REPORT_FOTA;
        break;

    case OTA_PLAT_REPORT_FOTA:
        /* 2. 上报网关版本 (设备级身份, 网关产品下) */
        otaReportVersionFota();
        s_st = OTA_PLAT_CHECK_FOTA;
        break;

    case OTA_PLAT_CHECK_FOTA:
        /* 3. 检测 FOTA (网关) 任务 */
        if (otaCheckTask(&OTA_ID_FOTA, OTA_TYPE_FOTA, GW_FW_VERSION))
        {
            s_st = OTA_PLAT_DOWNLOAD;
            break;
        }
        s_st = OTA_PLAT_CHECK_SOTA;
        break;

    case OTA_PLAT_CHECK_SOTA:
        /* 4. 检测 SOTA (节点) 任务, 版本用节点上报的最新版本 */
        if (otaCheckTask(&OTA_ID_SOTA, OTA_TYPE_SOTA, otaNodeCurVersion()))
        {
            /* 门控: 需 App 下发 OtaAllow=1 确认后才执行 (全网一键升级) */
            if (!s_otaAllow)
            {
                s_pendingSota = true;
                s_st = OTA_PLAT_IDLE;
                DBG_PRINTLN("[OTA][平台] 检测到升级任务, 等待App确认(OtaAllow=1)");
                break;
            }
            s_pendingSota = false;
            s_st = OTA_PLAT_DOWNLOAD;
            break;
        }
        s_pendingSota = false;
        DBG_PRINTLN("[OTA][平台] 无升级任务");
        s_st = OTA_PLAT_IDLE;
        break;

    case OTA_PLAT_DOWNLOAD:
    {
        ota_progressSet(0);             /* 固件下载中 (下载快, 进度置0) */
        const OtaIdentity_t *id = otaTaskIdentity();
        const char *fwFile = (s_taskType == OTA_TYPE_FOTA) ? OTA_GW_FILE : OTA_FW_FILE;
        long n = otaDownloadFile(id, s_tid, fwFile);
        if (n <= 0)
        {
            otaReportStatus(s_tid, 5);          /* 失败 */
            s_stateResetAt = millis();          /* 保持失败态片刻后复位属性 */
            s_st = OTA_PLAT_RESET;
            break;
        }
        s_st = OTA_PLAT_DISPATCH;
        break;
    }

    case OTA_PLAT_DISPATCH:
        if (s_taskType == OTA_TYPE_FOTA)
        {
            /* 网关自升级 (内部重启, 不再返回); 状态在 otaApplyGatewayFirmware 内设置 */
            otaApplyGatewayFirmware();
            s_st = OTA_PLAT_IDLE;               /* 升级失败时回到空闲 */
        }
        else
        {
            /* 节点固件: 校验文件头 → LoRa 链路下发 */
            if (otaValidateNodeFirmware())
            {
                uint8_t node = otaTargetNode();
                if (node != 0)
                {
                    if (ota_startFromFile(node, s_taskVer.c_str()))
                    {
                        ota_progressSet(0);     /* 升级中: LoRa 分发开始, 进度从0缓走 */
                        s_st = OTA_PLAT_FINISH;     /* 等 LoRa 链路完成 */
                    }
                    else
                        s_st = OTA_PLAT_IDLE;
                }
                else
                    s_st = OTA_PLAT_IDLE;
            }
            else
            {
                DBG_PRINTLN("[OTA][平台] 节点固件校验失败(魔数/长度不符)");
                otaReportStatus(s_tid, 5);
                s_stateResetAt = millis();        /* 保持失败态片刻后复位属性 */
                s_st = OTA_PLAT_RESET;
            }
        }
        break;

    case OTA_PLAT_FINISH:
        /* 等 LoRa OTA 链路跑完, 再报平台状态 */
        if (ota_getState() != OTA_IDLE)
            break;
        {
            /* 链路结束: 用 sentBytes 是否等于 totalBytes 粗略判断结果 */
            const OtaProgress_t *p = ota_getProgress();
            bool ok = (p->totalBytes > 0 && p->sentBytes >= p->totalBytes);
            DBG_PRINTF("[OTA][平台] LoRa 链路结束, %s, 上报平台\n",
                       ok ? "成功" : "失败");
            ota_progressSet(ok ? 100 : s_otaProgress);  /* 完成置满/失败保持当前进度 */
            otaReportStatus(s_tid, ok ? 201 : 5);
            s_stateResetAt = millis();            /* 保持终端态片刻后复位属性 */
        }
        s_st = OTA_PLAT_RESET;
        break;

    case OTA_PLAT_RESET:
        /* 完成/失败态保持 OTA_STATE_RESET_MS 后再复位 OtaProgress:
         * 立即复位 → onenet_loop 永远上报不到 progress=100, App 看不到"升级完成";
         * 不复位   → 下一次升级 App 直接读到旧的 progress=100, 进度条误显满格.
         * 复位为 0 与网关初始默认一致, 供下一次升级干净开始. */
        if (millis() - s_stateResetAt < OTA_STATE_RESET_MS)
            break;
        ota_progressSet(0);   /* 清零: OtaProgress=0 */
        ota_gateReset();
        s_st = OTA_PLAT_IDLE;
        break;
    }
}
