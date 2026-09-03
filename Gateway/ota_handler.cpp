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

        /* 喂狗 (ESP8266 无硬件看门狗, 但 yield 允许 WiFi 后台处理) */
        yield();
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

    /* 保存 URL */
    strncpy(s_otaUrl, url ? url : "", sizeof(s_otaUrl) - 1);
    s_otaUrl[sizeof(s_otaUrl) - 1] = '\0';

    s_otaStart = millis();
    s_hasResp = false;
    s_seq = 1;
    s_triggerAcked = false;
    s_triggerSendCount = 0;
    s_triggerSentAt = 0;

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

    s_otaUrl[0] = '\0';
    s_otaStart = millis();
    s_hasResp = false;
    s_seq = 1;
    s_triggerAcked = false;
    s_triggerSendCount = 0;
    s_triggerSentAt = 0;
    s_fileReady = true;

    DBG_PRINTF("[OTA] 启动(平台固件): 节点%d, 版本=%s\n",
               nodeId, s_progress.version);
    setState(OTA_TRIGGER_NODE);
    return true;
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

    /* ---------- 阶段7: 发送 EOT ---------- */
    case OTA_SEND_EOT:
    {
        uint8_t eot = OTA_EOT;
        sendRawFrame((uint16_t)s_progress.nodeId, LORA_CHANNEL, &eot, 1);
        DBG_PRINTLN("[OTA] EOT 已发送, 等待最终确认");
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
        /* 清理 */
        if (s_fwFile) s_fwFile.close();
        LittleFS.remove(OTA_FW_FILE);
        DBG_PRINTF("[OTA] 完成, 耗时 %lu 秒\n",
                   (unsigned long)(s_progress.elapsedMs / 1000));
        /* ⭐ 升级成功后, 重新标记节点需要下发阈值:
         * 新固件可能丢失了之前的阈值配置(Flash布局变化/默认值不同),
         * 节点重新上线(PONG)时自动重新下发之前保存的阈值 */
        {
            int slot = findNode((uint8_t)s_progress.nodeId);
            if (slot >= 0 && nodes[slot].thresholdValue > 0)
            {
                nodes[slot].thresholdNeedsUpdate = true;
                nodes[slot].thresholdRetryCount = 0;
                DBG_PRINTF("[OTA] 节点%d 升级成功, 标记重新下发僵尸车阈值=%lu秒\n",
                           s_progress.nodeId, (unsigned long)nodes[slot].thresholdValue);
            }
            /* ⭐ 超声波距离阈值同理重新标记下发 */
            if (slot >= 0 && nodes[slot].sensorDistanceValue > 0)
            {
                nodes[slot].sensorDistanceNeedsUpdate = true;
                nodes[slot].sensorDistanceRetryCount = 0;
                DBG_PRINTF("[OTA] 节点%d 升级成功, 标记重新下发超声波距离阈值=%ucm\n",
                           s_progress.nodeId, nodes[slot].sensorDistanceValue);
            }
        }
        s_progress.state = OTA_IDLE;
        break;

    case OTA_FAILED:
        /* 清理 */
        if (s_fwFile) s_fwFile.close();
        LittleFS.remove(OTA_FW_FILE);
        DBG_PRINTF("[OTA] 失败, 耗时 %lu 秒\n",
                   (unsigned long)(s_progress.elapsedMs / 1000));
        s_progress.state = OTA_IDLE;
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
        s_progress.state = OTA_IDLE;
    }
}

/* ⭐ 通知 OTA 处理器: 节点已回复 AT+OTA:ack
 * 由 lora_handler 在收到节点 ACK 帧时调用,
 * 让 OTA_TRIGGER_NODE 状态确认命令已送达, 进入复位等待 */
void ota_notifyTriggerAck(void)
{
    if (s_progress.state == OTA_TRIGGER_NODE)
        s_triggerAcked = true;
}