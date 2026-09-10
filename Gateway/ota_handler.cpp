/* ota_handler.cpp - Gateway OTA 升级处理器实现
 *
 * 状态机说明:
 *   OTA_IDLE → OTA_TRIGGER_NODE → OTA_WAIT_NODE_RESET → OTA_DOWNLOADING
 *   → OTA_SENDING → OTA_WAIT_ACK → (循环 SENDING/WAIT_ACK)
 *   → OTA_SEND_EOT → OTA_WAIT_FINAL_ACK → OTA_COMPLETE / OTA_FAILED
 *
 * 非阻塞设计: 每个状态只在 ota_tick() 中执行一步, 不阻塞主循环.
 * 下载使用 HTTPClient 流式写入 LittleFS, 发送使用 LoRa 串口直接发送.
 */
#include "ota_handler.h"
#include "lora_protocol.h"
#include "hw_cfg.h"
#include "app_cfg.h"
#include "node_data.h"    /* ⭐ findNode/nodes: OTA成功后重新标记阈值下发 */
#include "onenet_handler.h" /* ⭐ S27: 下载期间让出 MQTT 保活/收下行 */

#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <LittleFS.h>

/* ==================== 内部数据结构 ==================== */

/* 固件文件头 (12B, 与 Tool/fw_pack.py 一致) */
typedef struct __attribute__((packed)) {
    uint16_t magic;      /* OTA_FW_MAGIC (0xA55A) */
    uint16_t version;    /* 固件版本号 */
    uint32_t length;     /* 代码段长度(字节) */
    uint32_t crc32;      /* 代码段 CRC32 */
} FwHeader_t;

/* Xmodem 发送缓冲区 */
#define XM_PACKET_OVERHEAD  5       /* SOH(1) + Seq(2) + CRC(2) */
#define XM_PACKET_TOTAL     (XM_PACKET_OVERHEAD + OTA_PACKET_DATA_SIZE)  /* 133 */

/* ==================== 静态变量 ==================== */

static OtaProgress_t s_progress;        /* 进度信息 */
static File s_fwFile;                   /* 固件文件句柄 */
static uint16_t s_seq;                  /* 当前包序号 (从1开始) */
static uint8_t s_pktBuf[XM_PACKET_TOTAL];  /* Xmodem 包缓冲区 */
static uint32_t s_stateStart;           /* 当前状态进入时间戳 */
static uint32_t s_otaStart;             /* OTA开始时间戳 */
static uint8_t s_otaResp;               /* 收到的 OTA 响应字节 */
static volatile bool s_hasResp;         /* 是否已收到 OTA 响应 */

/* ⭐ AT+OTA 触发命令可靠投递: 发命令后等节点 ACK, 丢包则重发 */
static volatile bool s_triggerAcked;    /* 节点已回复 AT+OTA:ack */
static uint8_t  s_triggerSendCount;     /* AT+OTA 已发送次数(含首次) */
static uint32_t s_triggerSentAt;        /* 上次发送 AT+OTA 的时间戳 */

/* ⭐ 整链自动重试 (设计文档 3.3.2): OTA_FAILED 后整链重来次数(不含首次) */
static uint8_t  s_chainRetry;

/* ⭐ S21: 节点拒绝升级标志 (收到 AT+OTA:version_ok 置位).
 * 置位时 OTA_FAILED 不再整链重试(节点 App 已明确拒绝, 重试只会再次
 * version_ok 空转), 直接回 OTA_IDLE 让平台侧上报失败 */
static bool     s_refused;

/* ⭐ S27 自愈预算: 同一节点"连续自动重发自愈"失败次数累计.
 * 根因治理: 历史无脑循环 = 失败耗尽回 OTA_IDLE 后判定器(ota_autoDispatch)
 * 见固件文件即自动重发 → 无限循环. 达 OTA_SELF_HEAL_MAX_RETRY 次后停止自动,
 * 只置 bootPending 等平台/人工显式触发(ota_start / ota_startFromFile 显式入口
 * 清零, ota_autoDispatch 置 s_selfHealRun 标记来源区分) */
static uint8_t  s_selfHealNode   = 0xFF;  /* 正在累计预算的节点 */
static uint8_t  s_selfHealFails  = 0;     /* 该节点连续自愈失败次数 */
static bool     s_selfHealRun    = false; /* 本次 OTA 是否由自动自愈发起 */

/* ⭐ S27: 自动自愈发起的 OTA 流程终结(失败/被拒) → 累计自愈预算.
 * 整链重试中途不累计, 只有最终回 OTA_IDLE 才算一次失败 */
static void selfHealFail(void)
{
    if (!s_selfHealRun) return;
    s_selfHealRun = false;
    s_selfHealNode = (uint8_t)s_progress.nodeId;
    s_selfHealFails++;
    DBG_PRINTF("[OTA] 自愈失败累计: 节点%d 第%u/%u次 (达上限后判定器停止自动重发)\n",
               s_progress.nodeId, (unsigned)s_selfHealFails,
               (unsigned)OTA_SELF_HEAL_MAX_RETRY);
}

/* CRC16 表 (XMODEM poly 0x1021, 与节点端一致) */
static const uint16_t s_crc16Table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

/* ==================== CRC16-XMODEM (查表法) ==================== */
static uint16_t crc16_xmodem(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0;
    for (uint32_t i = 0; i < len; i++)
        crc = (crc << 8) ^ s_crc16Table[((crc >> 8) ^ data[i]) & 0xFF];
    return crc;
}

/* ==================== LoRa 发送原始数据 ==================== */
/* 向目标节点发送原始字节 (无帧头, 直接定点传输), 用于 OTA 数据包 */
static void sendRawFrame(uint16_t dstAddr, uint8_t ch,
                         const uint8_t *data, uint16_t len)
{
    extern Stream &lora_getSerial(void);
    Stream &lora = lora_getSerial();

    uint8_t header[3];
    header[0] = (uint8_t)(dstAddr >> 8);
    header[1] = (uint8_t)(dstAddr & 0xFF);
    header[2] = ch;
    lora.write(header, 3);
    if (len > 0) lora.write(data, len);
}

/* ==================== 状态转换辅助 ==================== */
static void setState(OtaState_t st)
{
    s_progress.state = st;
    s_stateStart = millis();
}

/* ==================== 下载固件 ==================== */
static bool downloadFirmware(const char *url)
{
    DBG_PRINTF("[OTA] 开始下载固件: %s\n", url);

    /* 确保 LittleFS 已挂载 */
    if (!LittleFS.begin())
    {
        DBG_PRINTLN("[OTA] LittleFS 挂载失败");
        return false;
    }

    /* 删除旧文件 */
    LittleFS.remove(OTA_FW_FILE);

    WiFiClient wc;
    HTTPClient http;
    http.begin(wc, url);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        DBG_PRINTF("[OTA] 下载失败, HTTP %d\n", httpCode);
        http.end();
        return false;
    }

    int totalSize = http.getSize();
    s_progress.totalBytes = (totalSize > 0) ? (uint32_t)totalSize : 0;

    /* 打开文件准备写入 */
    s_fwFile = LittleFS.open(OTA_FW_FILE, "w");
    if (!s_fwFile)
    {
        DBG_PRINTLN("[OTA] 无法创建固件文件");
        http.end();
        return false;
    }

    /* 流式写入 */
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[256];
    int written = 0;
    int lastPct = -1;

    while (http.connected() && stream->available())
    {
        int len = stream->readBytes(buf, sizeof(buf));
        if (len > 0)
        {
            s_fwFile.write(buf, len);
            written += len;
            s_progress.sentBytes = (uint32_t)written;

            /* 进度日志 */
            if (s_progress.totalBytes > 0)
            {
                int pct = (written * 100) / (int)s_progress.totalBytes;
                if (pct != lastPct && (pct % 25 == 0 || pct == 100))
                {
                    DBG_PRINTF("[OTA] 下载进度: %d%% (%d/%d)\n",
                               pct, written, s_progress.totalBytes);
                    lastPct = pct;
                }
            }
        }

        /* ⭐ S27: 下载期间周期让出 MQTT 保活/收下行, 不再裸 yield()
         * (历史: 同步下载阻塞主循环期间 MQTT 停顿, 平台命令接收迟钝) */
        onenet_loop();
    }

    s_fwFile.close();
    http.end();

    DBG_PRINTF("[OTA] 下载完成: %d 字节\n", written);

    if (written == 0)
    {
        LittleFS.remove(OTA_FW_FILE);
        return false;
    }

    s_progress.totalBytes = (uint32_t)written;
    s_progress.sentBytes = 0;
    return true;
}

/* ==================== 发送一包 Xmodem 数据 ==================== */
static bool sendXmodemPacket(void)
{
    uint8_t data[OTA_PACKET_DATA_SIZE];

    /* 从文件读取 128 字节 */
    int bytesRead = s_fwFile.read(data, OTA_PACKET_DATA_SIZE);
    if (bytesRead <= 0)
    {
        /* 文件读完了 */
        return false;
    }

    /* 不足 128B 的用 0x1A (Ctrl-Z) 填充 (Xmodem 规范) */
    if (bytesRead < OTA_PACKET_DATA_SIZE)
        memset(data + bytesRead, 0x1A, OTA_PACKET_DATA_SIZE - bytesRead);

    /* 计算 CRC16 */
    uint16_t crc = crc16_xmodem(data, OTA_PACKET_DATA_SIZE);

    /* 组装 Xmodem 包 */
    s_pktBuf[0] = OTA_SOH;                              /* SOH */
    s_pktBuf[1] = (uint8_t)(s_seq >> 8);                 /* SeqH */
    s_pktBuf[2] = (uint8_t)(s_seq & 0xFF);                /* SeqL */
    memcpy(s_pktBuf + 3, data, OTA_PACKET_DATA_SIZE);    /* 128B 数据 */
    s_pktBuf[3 + OTA_PACKET_DATA_SIZE] = (uint8_t)(crc >> 8);     /* CRCH */
    s_pktBuf[3 + OTA_PACKET_DATA_SIZE + 1] = (uint8_t)(crc & 0xFF); /* CRCL */

    /* 通过 LoRa 发送 (定点传输到目标节点) */
    sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL,
                 s_pktBuf, XM_PACKET_TOTAL);

    s_progress.sentBytes += (uint32_t)bytesRead;
    s_progress.retryCount = 0;

    /* 进度日志: 首包 + 每10包打印一次 */
    {
        uint16_t totalPkts = (uint16_t)((s_progress.totalBytes + OTA_PACKET_DATA_SIZE - 1) / OTA_PACKET_DATA_SIZE);
        if (s_seq == 1 || (s_seq % 10) == 0)
        {
            int pct = (totalPkts > 0) ? (int)(s_seq * 100 / totalPkts) : 0;
            DBG_PRINTF("[OTA] 发包 #%u/%u (%d%%) %u/%uB\n",
                       (unsigned)s_seq, (unsigned)totalPkts, pct,
                       (unsigned)s_progress.sentBytes,
                       (unsigned)s_progress.totalBytes);
        }
    }

    return true;
}

/* ==================== 文件级静态变量 ==================== */
static char s_otaUrl[256];  /* 固件下载 URL, 由 ota_start 设置, ota_tick 使用 */
static bool s_fileReady = false;  /* true=固件已就绪于 OTA_FW_FILE, 跳过下载 */

/* ==================== 公开函数 ==================== */

void ota_init(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.state = OTA_IDLE;
    s_hasResp = false;
    s_fwFile = File();
    DBG_PRINTLN("[OTA] 处理器初始化完成");
}

bool ota_start(uint8_t nodeId, const char *url, const char *version)
{
    if (s_progress.state != OTA_IDLE)
    {
        DBG_PRINTF("[OTA] 启动失败: 当前状态=%d (忙)\n", s_progress.state);
        return false;
    }

    /* 保存参数 */
    s_progress.nodeId = nodeId;
    s_progress.totalBytes = 0;
    s_progress.sentBytes = 0;
    s_progress.retryCount = 0;
    s_progress.elapsedMs = 0;
    strncpy(s_progress.version, version ? version : "V0.0", sizeof(s_progress.version) - 1);
    s_progress.version[sizeof(s_progress.version) - 1] = '\0';

    /* ⭐ S14: OTA 显式启动 → 清除该节点"等平台"标志 (8.2: 平台下发时立即执行);
     * ⭐ S21: 同时清除"拒绝升级"标志 (人工重触发/平台再下发 = 新一次升级意图) */
    {
        int slot = findNode(nodeId);
        if (slot >= 0)
        {
            nodes[slot].bootPending = false;
            nodes[slot].otaRefused  = false;
        }
    }

    /* 保存 URL */
    strncpy(s_otaUrl, url ? url : "", sizeof(s_otaUrl) - 1);
    s_otaUrl[sizeof(s_otaUrl) - 1] = '\0';

    s_otaStart = millis();
    s_hasResp = false;
    s_seq = 1;
    s_triggerAcked = false;
    s_triggerSendCount = 0;
    s_triggerSentAt = 0;
    s_chainRetry = 0;

    DBG_PRINTF("[OTA] 启动: 节点%d, 版本=%s, URL=%s\n", nodeId, s_progress.version, s_otaUrl);

    /* 进入触发节点状态 */
    s_fileReady = false;
    setState(OTA_TRIGGER_NODE);
    return true;
}

/* 固件已下载到 OTA_FW_FILE, 跳过下载直接下发 */
bool ota_startFromFile(uint8_t nodeId, const char *version)
{
    if (s_progress.state != OTA_IDLE)
    {
        DBG_PRINTF("[OTA] 启动失败: 当前状态=%d (忙)\n", s_progress.state);
        return false;
    }
    if (!LittleFS.begin() || !LittleFS.exists(OTA_FW_FILE))
    {
        DBG_PRINTLN("[OTA] 固件文件不存在");
        return false;
    }

    s_progress.nodeId = nodeId;
    s_progress.totalBytes = 0;
    s_progress.sentBytes = 0;
    s_progress.retryCount = 0;
    s_progress.elapsedMs = 0;
    strncpy(s_progress.version, version ? version : "V0.0",
            sizeof(s_progress.version) - 1);
    s_progress.version[sizeof(s_progress.version) - 1] = '\0';

    /* ⭐ S14: OTA 显式启动 → 清除该节点"等平台"标志 (平台下发路径同 ota_start);
     * ⭐ S21: 同时清除"拒绝升级"标志 */
    {
        int slot = findNode(nodeId);
        if (slot >= 0)
        {
            nodes[slot].bootPending = false;
            nodes[slot].otaRefused  = false;
        }
        /* ⭐ S27: 显式触发(平台固件分发/人工)清零自愈预算, 恢复自动自愈;
         * 自动自愈(autoDispatch 置 s_selfHealRun)重发则保留预算供下一轮判定 */
        if (!s_selfHealRun)
        {
            if (s_selfHealNode == nodeId) s_selfHealFails = 0;
        }
        s_selfHealRun = false;
    }

    s_otaUrl[0] = '\0';
    s_otaStart = millis();
    s_hasResp = false;
    s_seq = 1;
    s_triggerAcked = false;
    s_triggerSendCount = 0;
    s_triggerSentAt = 0;
    s_chainRetry = 0;
    s_fileReady = true;

    DBG_PRINTF("[OTA] 启动(平台固件): 节点%d, 版本=%s\n",
               nodeId, s_progress.version);
    setState(OTA_TRIGGER_NODE);
    return true;
}

/* ⭐ S14/8.1 统一判定器: 节点 mode=BOOT(收到 PONG,BOOT)时调用.
 * 单一决策入口, 自动(节点自曝)/人工/平台触发全部汇入 ota_start 单轨:
 *   - OTA 忙 → 不动作 (升级/整链重试/下载中, 现有机制接管)
 *   - 有固件文件 → ota_startFromFile 自动重发 (自愈: 升级成功滞留 Boot 也成立)
 *   - 无固件文件 → 置节点 bootPending=true, 等平台下发 (8.2) */
bool ota_autoDispatch(uint8_t nodeId)
{
    if (s_progress.state != OTA_IDLE)
    {
        DBG_PRINTF("[OTA] 判定器: 节点%d 在Boot但OTA忙(state=%d), 不动作\n",
                   nodeId, s_progress.state);
        return false;
    }

    int slot = findNode(nodeId);
    bool wasPending = (slot >= 0) ? nodes[slot].bootPending : false;  /* ⭐ P1-4: 记录置位前状态 */
    if (slot >= 0) nodes[slot].bootPending = false;   /* 有决策动作, 先清等待标志 */

    if (LittleFS.begin() && LittleFS.exists(OTA_FW_FILE))
    {
        DBG_PRINTF("[OTA] 判定器: 节点%d 在Boot且有固件文件, 自动重发(自愈)\n", nodeId);
        return ota_startFromFile(nodeId, NULL);
    }

    if (slot >= 0)
    {
        nodes[slot].bootPending = true;   /* 无固件: 标记等平台下发 */
        /* ⭐ P1-4 降噪: 每轮 PING(~2s) 重复置位会无限刷屏,
         * 仅首次由非等待 → 等待(等平台下发)时打印一次 */
        if (!wasPending)
            DBG_PRINTF("[OTA] 判定器: 节点%d 在Boot且无固件, bootPending=true 等平台下发\n",
                       nodeId);
    }
    else
        DBG_PRINTF("[OTA] 判定器: 节点%d 未注册, 忽略\n", nodeId);
    return false;
}

/* ⭐ S14 人工触发统一入口 (串口 OTA 命令走此, 不再直接调 ota_start):
 * 与自动路径同轨防双轨并发:
 *   - OTA 忙 → 拒绝
 *   - 节点已在 Boot 且网关有固件 → 判定器文件自愈重发 (ota_autoDispatch)
 *   - 其余 (APP/UNKNOWN 节点, 或 Boot 但无文件) → ota_start 下载 URL 固件触发 */
bool ota_manualTrigger(uint8_t nodeId, const char *url, const char *version)
{
    if (s_progress.state != OTA_IDLE)
    {
        DBG_PRINTF("[OTA] 手动触发被拒: 当前状态=%d (忙, 等 OTA 结束后再试)\n",
                   s_progress.state);
        return false;
    }

    int slot = findNode(nodeId);
    if (slot >= 0 && nodes[slot].mode == NODE_MODE_BOOT && ota_autoDispatch(nodeId))
        return true;   /* Boot 节点已有固件: 走自愈重发, 不重新下载 */

    return ota_start(nodeId, url, version);
}

/* OTA 状态机主循环 */
void ota_tick(void)
{
    uint32_t now = millis();
    s_progress.elapsedMs = now - s_otaStart;

    switch (s_progress.state)
    {
    case OTA_IDLE:
        /* 什么都不做 */
        break;

    /* ---------- 阶段1: 触发节点复位进 BootLoader ----------
     * ⭐ 可靠投递: 发 AT+OTA 后等节点回复 ACK, 丢包则重发(最多3次),
     * 收到 ACK 才进入复位等待, 避免节点没收到命令导致盲等超时 */
    case OTA_TRIGGER_NODE:
    {
        /* 首次进入: 等 500ms LoRa 信道空闲再发 AT+OTA
         * (ota_startFromFile 通常在节点刚回复 DATA 后被调用,
         *  节点 LoRa 模块可能还在 TX 模式, 立即发会因半双工冲突丢失) */
        if (s_triggerSentAt == 0)
        {
            if (now - s_stateStart < 500)
                break;
            char cmd[LORA_CMD_MAX_LEN];
            int n = snprintf(cmd, sizeof(cmd), "AT+OTA=start,%s\r\n", s_progress.version);
            if (n > 0)
            {
                sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL,
                             (const uint8_t *)cmd, (uint16_t)n);
                DBG_PRINTF("[OTA] 触发节点%d: %s", s_progress.nodeId, cmd);
            }
            s_triggerSentAt = now;
            s_triggerSendCount = 1;
            break;
        }

        /* 收到节点 ACK: 确认命令已送达, 进入复位等待 */
        if (s_triggerAcked)
        {
            DBG_PRINTLN("[OTA] 节点已确认触发命令, 等待复位");
            setState(OTA_WAIT_NODE_RESET);
            break;
        }

        /* 等待 ACK 超时: 重发 AT+OTA (防 LoRa 丢包), 最多发 OTA_TRIGGER_MAX_SEND 次 */
        if (now - s_triggerSentAt >= OTA_TRIGGER_ACK_TIMEOUT_MS)
        {
            if (s_triggerSendCount < OTA_TRIGGER_MAX_SEND)
            {
                char cmd[LORA_CMD_MAX_LEN];
                int n = snprintf(cmd, sizeof(cmd), "AT+OTA=start,%s\r\n", s_progress.version);
                if (n > 0)
                {
                    sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL,
                                 (const uint8_t *)cmd, (uint16_t)n);
                    DBG_PRINTF("[OTA] 触发节点%d (第%d次, 无ACK重发): %s",
                               s_progress.nodeId, s_triggerSendCount + 1, cmd);
                }
                s_triggerSentAt = now;
                s_triggerSendCount++;
            }
            else
            {
                DBG_PRINTF("[OTA] 触发节点%d 失败: 无ACK, 已发%d次\n",
                           s_progress.nodeId, s_triggerSendCount);
                setState(OTA_FAILED);
            }
        }
        break;
    }

    /* ---------- 阶段2: 等待节点复位进 BootLoader ---------- */
    case OTA_WAIT_NODE_RESET:
        if (now - s_stateStart >= OTA_NODE_RESET_WAIT_MS)
        {
            DBG_PRINTLN("[OTA] 节点复位等待完成, 开始下载固件");
            setState(OTA_DOWNLOADING);
        }
        break;

    /* ---------- 阶段3: 下载固件到 LittleFS (已就绪则跳过) ---------- */
    case OTA_DOWNLOADING:
        if (s_fileReady)
        {
            /* 平台 OTA: 固件已下载, 直接打开发送 */
            s_fwFile = LittleFS.open(OTA_FW_FILE, "r");
            if (!s_fwFile)
            {
                DBG_PRINTLN("[OTA] 无法打开固件文件");
                setState(OTA_FAILED);
                break;
            }
            s_seq = 1;
            s_progress.sentBytes = 0;
            s_progress.totalBytes = (uint32_t)s_fwFile.size();
            DBG_PRINTF("[OTA] 平台固件已就绪, 大小=%d 字节, 开始分包发送\n",
                       s_fwFile.size());
            setState(OTA_SENDING);
            break;
        }
        if (downloadFirmware(s_otaUrl))
        {
            /* 打开文件准备读取 */
            s_fwFile = LittleFS.open(OTA_FW_FILE, "r");
            if (!s_fwFile)
            {
                DBG_PRINTLN("[OTA] 无法打开固件文件");
                setState(OTA_FAILED);
                break;
            }
            s_seq = 1;
            s_progress.sentBytes = 0;
            DBG_PRINTF("[OTA] 固件文件已打开, 大小=%d 字节, 开始分包发送\n",
                       s_fwFile.size());
            setState(OTA_SENDING);
        }
        else
        {
            DBG_PRINTLN("[OTA] 固件下载失败");
            setState(OTA_FAILED);
        }
        break;

    /* ---------- 阶段4: 发送 Xmodem 数据包 ---------- */
    case OTA_SENDING:
    {
        if (!sendXmodemPacket())
        {
            /* 文件读取完毕, 发送 EOT */
            DBG_PRINTF("[OTA] 所有数据包发送完毕 (%u 包), 发送 EOT\n",
                       (unsigned)(s_seq - 1));
            setState(OTA_SEND_EOT);
            break;
        }

        s_hasResp = false;
        setState(OTA_WAIT_ACK);
        break;
    }

    /* ---------- 阶段5: 等待 ACK/NAK ---------- */
    case OTA_WAIT_ACK:
        if (s_hasResp)
        {
            if (s_otaResp == OTA_ACK)
            {
                /* ACK: 下一包 */
                uint16_t totalPkts = (uint16_t)((s_progress.totalBytes + OTA_PACKET_DATA_SIZE - 1) / OTA_PACKET_DATA_SIZE);
                if ((s_seq % 10) == 0 || s_seq == 1)
                {
                    int pct = (totalPkts > 0) ? (int)(s_seq * 100 / totalPkts) : 0;
                    DBG_PRINTF("[OTA] ACK #%u/%u (%d%%)\n",
                               (unsigned)s_seq, (unsigned)totalPkts, pct);
                }
                s_seq++;
                s_progress.retryCount = 0;
                setState(OTA_SENDING);
            }
            else if (s_otaResp == OTA_NAK)
            {
                /* NAK: 重发当前包 */
                DBG_PRINTF("[OTA] 节点NAK, 重发包 #%u\n", (unsigned)s_seq);
                s_progress.retryCount++;
                if (s_progress.retryCount >= OTA_MAX_RETRY)
                {
                    DBG_PRINTF("[OTA] 包 #%u 重试次数超限\n", (unsigned)s_seq);
                    setState(OTA_FAILED);
                }
                else
                {
                    setState(OTA_RETRY_PACKET);
                }
            }
            else if (s_otaResp == OTA_CAN)
            {
                DBG_PRINTLN("[OTA] 节点取消传输");
                setState(OTA_FAILED);
            }
            s_hasResp = false;
        }
        else if (now - s_stateStart >= OTA_PACKET_TIMEOUT_MS)
        {
            /* 超时: 重发 */
            DBG_PRINTF("[OTA] 等待ACK超时, 重发包 #%u\n", (unsigned)s_seq);
            s_progress.retryCount++;
            if (s_progress.retryCount >= OTA_MAX_RETRY)
            {
                DBG_PRINTF("[OTA] 包 #%u 重试次数超限\n", (unsigned)s_seq);
                setState(OTA_FAILED);
            }
            else
            {
                setState(OTA_RETRY_PACKET);
            }
        }
        break;

    /* ---------- 阶段6: 重发当前包 ---------- */
    case OTA_RETRY_PACKET:
    {
        /* 重新发送上一包 (s_pktBuf 中还有数据) */
        sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL,
                     s_pktBuf, XM_PACKET_TOTAL);
        s_hasResp = false;
        setState(OTA_WAIT_ACK);
        break;
    }

    /* ---------- 阶段7: 发送 EOT (⭐ S26 双EOT确认) ----------
     * 节点端 OTA_ReceivePacket 对齐 CAN 分支, 需连续收到两个 0x04 才判
     * 传输结束 (防空口杂波/回环单字节 0x04 误判 EOT → 提前结束 → CRC32
     * 必败, 历史现象). 此处拆两个状态各发一个, 中间隔一个主循环 tick,
     * 避免背靠背粘连, 日志可区分 1/2. */
    case OTA_SEND_EOT:
    {
        uint8_t eot = OTA_EOT;
        sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL, &eot, 1);
        DBG_PRINTLN("[OTA] EOT 已发送(1/2), 等待最终确认");
        s_hasResp = false;
        setState(OTA_SEND_EOT2);
        break;
    }

    case OTA_SEND_EOT2:
    {
        uint8_t eot = OTA_EOT;
        sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL, &eot, 1);
        DBG_PRINTLN("[OTA] EOT 已发送(2/2), 等待最终确认");
        s_hasResp = false;
        setState(OTA_WAIT_FINAL_ACK);
        break;
    }

    /* ---------- 阶段8: 等待最终 ACK (CRC32 验证结果) ---------- */
    case OTA_WAIT_FINAL_ACK:
        if (s_hasResp)
        {
            if (s_otaResp == OTA_ACK)
            {
                DBG_PRINTLN("[OTA] 升级成功!");
                setState(OTA_COMPLETE);
            }
            else if (s_otaResp == OTA_NAK)
            {
                DBG_PRINTLN("[OTA] 升级失败 (CRC32 校验不通过)");
                setState(OTA_FAILED);
            }
            s_hasResp = false;
        }
        else if (now - s_stateStart >= OTA_PACKET_TIMEOUT_MS)
        {
            /* 最终 ACK 超时, 也算完成(节点可能已跳转) */
            DBG_PRINTLN("[OTA] 最终确认超时, 视为完成");
            setState(OTA_COMPLETE);
        }
        break;

    /* ---------- 完成 / 失败 ---------- */
    case OTA_COMPLETE:
        /* 关闭文件句柄 */
        if (s_fwFile) s_fwFile.close();
        /* ⭐ S15: 固件文件保留, 不立即删除!
         * 升级成功后节点可能滞留 Boot, 判定器(ota_autoDispatch)需要该文件
         * 自动重发才能完成自愈闭环. 文件在以下时机才清理:
         *   1. 节点确认离开 Boot (收到 PONG,APP → ota_notifyNodeApp)
         *   2. 被新固件覆盖 (平台/人工重新下载时 LittleFS.remove + 重写) */
        DBG_PRINTF("[OTA] 完成, 耗时 %lu 秒 (固件文件保留, 待节点确认离开Boot后清理)\n",
                   (unsigned long)(s_progress.elapsedMs / 1000));
        /* ⭐ S30: 删除"OTA 成功后重新标记下发"——升级后节点重启进 APP 必然回
         * PONG,APP, 已由 PONG 兜底重发(lora_handler PONG,APP 分支)统一覆盖,
         * 此处不再重复置位 (原置位逻辑与 LFS 加载置位一同收敛到 PONG 一处) */
        /* ⭐ S27: 自动自愈成功 → 清零该节点预算(连续失败归零) */
        if (s_selfHealRun)
        {
            s_selfHealRun = false;
            if (s_selfHealNode == (uint8_t)s_progress.nodeId)
                s_selfHealFails = 0;
            DBG_PRINTLN("[OTA] 自愈成功, 预算清零");
        }
        s_progress.state = OTA_IDLE;
        break;

    case OTA_FAILED:
        /* ⭐ S21: 节点拒绝升级(version_ok) → 不整链重试, 保留固件文件
         * (人工重触发自愈路径仍可用), 直接回 OTA_IDLE;
         * 平台侧 OTA_PLAT_FINISH 检测到 OTA_IDLE 即上报失败(202) */
        if (s_refused)
        {
            s_refused = false;
            DBG_PRINTF("[OTA] 节点%d 拒绝升级, 终止 OTA (固件文件保留, 待人工/平台介入)\n",
                       s_progress.nodeId);
            s_progress.state = OTA_IDLE;
            break;
        }
        /* ⭐ 整链自动重试 (设计文档 3.3.2): 失败不立即放弃,
         * 保留固件文件(LittleFS), 停 OTA_CHAIN_RETRY_INTERVAL_MS 后
         * 从 OTA_TRIGGER_NODE 整链重来(重新触发 → 节点复位 → 重新下发).
         * 重试次数耗尽也不删文件, 保留给判定器自动重发自愈 */
        if (s_fwFile) s_fwFile.close();
        if (s_chainRetry < OTA_CHAIN_MAX_RETRY)
        {
            s_chainRetry++;
            DBG_PRINTF("[OTA] 失败, 第%u/%u次整链重试 (保留固件文件, %lu秒后重发触发)\n",
                       (unsigned)s_chainRetry, (unsigned)OTA_CHAIN_MAX_RETRY,
                       (unsigned long)(OTA_CHAIN_RETRY_INTERVAL_MS / 1000));
            setState(OTA_RETRY_WAIT);
            break;
        }
        /* ⭐ P0-3 失败重试耗尽不再删文件: 保留固件文件, 平台上报失败(202)后,
         * 判定器(ota_autoDispatch)在节点 PONG,BOOT 时仍可自动重发(自愈),
         * 避免"节点可救却因文件被删而永久僵持"(历史死锁根因之一).
         * 文件仍在下述时机清理:
         *   1. 节点确认离开 Boot (PONG,APP → ota_notifyNodeApp)
         *   2. 被新固件覆盖 (平台/人工重新下载时 LittleFS.remove + 重写) */
        DBG_PRINTF("[OTA] 失败, 耗时 %lu 秒 (重试%u次已耗尽, 固件文件保留待自愈重发)\n",
                   (unsigned long)(s_progress.elapsedMs / 1000),
                   (unsigned)OTA_CHAIN_MAX_RETRY);
        selfHealFail();     /* ⭐ S27: 自动自愈失败耗尽 → 累计预算 */
        s_progress.state = OTA_IDLE;
        break;

    /* ---------- 整链重试等待: 间隔后整链重来 ---------- */
    case OTA_RETRY_WAIT:
        if (now - s_stateStart < OTA_CHAIN_RETRY_INTERVAL_MS)
            break;
        /* 重置整链状态, 从触发节点开始重新发起 */
        s_triggerAcked = false;
        s_triggerSendCount = 0;
        s_triggerSentAt = 0;
        s_seq = 1;
        s_hasResp = false;
        s_progress.sentBytes = 0;
        s_progress.retryCount = 0;
        DBG_PRINTF("[OTA] 整链重试: 重新触发节点%d\n", s_progress.nodeId);
        setState(OTA_TRIGGER_NODE);
        break;
    }
}

/* 喂 OTA 响应字节 (从 LoRa RX 状态机调用) */
void ota_feedByte(uint8_t b)
{
    if (s_progress.state == OTA_WAIT_ACK ||
        s_progress.state == OTA_WAIT_FINAL_ACK)
    {
        if (b == OTA_ACK || b == OTA_NAK || b == OTA_CAN)
        {
            s_otaResp = b;
            s_hasResp = true;
        }
    }
}

const OtaProgress_t *ota_getProgress(void)
{
    return &s_progress;
}

OtaState_t ota_getState(void)
{
    return s_progress.state;
}

void ota_cancel(void)
{
    if (s_progress.state != OTA_IDLE)
    {
        if (s_fwFile) s_fwFile.close();
        LittleFS.remove(OTA_FW_FILE);
        DBG_PRINTLN("[OTA] 已取消");
        /* ⭐ S27: 人工取消视为介入, 清零自愈预算 */
        s_selfHealRun = false;
        s_selfHealFails = 0;
        s_progress.state = OTA_IDLE;
    }
}

/* ⭐ 通知 OTA 处理器: 节点已回复 AT+OTA:ack
 * 由 lora_handler 在收到节点 ACK 帧时调用,
 * 让 OTA_TRIGGER_NODE 状态确认命令已送达, 进入复位等待.
 * ⭐ P1-5: 校验确认必须来自最近触发命令的目标节点,
 * 防止多节点并存时他节点 ACK 被误当本节点触发确认 */
void ota_notifyTriggerAck(uint8_t nodeId)
{
    if (s_progress.nodeId != nodeId)
    {
        DBG_PRINTF("[OTA] 收到节点%d 触发ACK, 但最近触发目标为%d, 忽略\n",
                   nodeId, s_progress.nodeId);
        return;
    }
    if (s_progress.state == OTA_TRIGGER_NODE)
        s_triggerAcked = true;
}

/* ⭐ S21: 节点 App 回复 AT+OTA:version_ok (版本已最新, 拒绝升级)
 * 由 lora_handler 在收到该 ACK 帧时调用.
 * 语义 (方案 8.1#2): 标记节点 otaRefused=true (待人工/平台介入) →
 * 终止对 App 的 Xmodem 流 → 平台侧经 OTA_PLAT_FINISH 上报失败(202).
 * 不做整链重试: 节点 App 已明确拒绝, 重试只会再次 version_ok 空转. */
void ota_notifyVersionOk(uint8_t nodeId)
{
    if (s_progress.nodeId != nodeId)
    {
        DBG_PRINTF("[OTA] 收到节点%d version_ok, 但最近 OTA 目标为%d, 忽略\n",
                   nodeId, s_progress.nodeId);
        return;
    }
    int slot = findNode(nodeId);
    if (slot >= 0)
    {
        nodes[slot].otaRefused = true;
        DBG_PRINTF("[OTA] 节点%d 拒绝升级(version_ok), otaRefused=true 待人工/平台介入\n",
                   nodeId);
    }
    if (s_progress.state != OTA_IDLE)
    {
        s_refused = true;
        setState(OTA_FAILED);
    }
}

/* ⭐ S15: 节点确认离开 Boot (收到 PONG,APP) → 清理固件文件.
 * 仅当该节点是最近一次 OTA 目标节点时删除, 避免误删其他节点固件;
 * 文件保留期内的自愈重发(判定器)由此画上句号 */
void ota_notifyNodeApp(uint8_t nodeId)
{
    if (s_progress.nodeId != nodeId) return;
    if (!LittleFS.begin() || !LittleFS.exists(OTA_FW_FILE)) return;
    LittleFS.remove(OTA_FW_FILE);
    DBG_PRINTF("[OTA] 节点%d 确认进入 APP 模式, 清理固件文件 (自愈闭环结束)\n",
               nodeId);
}