/* lora_handler.cpp - Gateway LoRa 定点传输 + PING 快速轮询
 *
 * 轮询策略:
 *   1. 已注册在线节点: 直接 AT+DATA (快速路径, 不经过 PING)
 *   2. 已注册离线节点: 先 AT+PING (短超时 500ms), 通了再 AT+DATA
 *   3. 搜索模式(每次开机 / 按 FLASH 按钮): 对每个地址逐一 AT+PING 探测,
 *      PING 通: 无证书 → AT+CER 注册; 有证书 → 跳过(CER 已有, 代上线由
 *      loginPending + uploadAll 定时处理); 不通直接下一个;
 *      扫完一轮自动退出; 平时空槽位直接跳过不打扰
 *   4. PING 超时:      跳过, 下一轮再查
 *   5. 控制命令:       优先于轮询, 插入即发
 *
 * 这样平时只轮询已注册节点, 空口更干净; 按 FLASH 短按可随时重扫发现新节点.
 */
#include "lora_handler.h"
#include "hw_cfg.h"       /* LoRa 串口引脚/波特率/调试串口 */
#include "app_cfg.h"      /* 轮询范围/超时/发现间隔/DBG */
#include "node_data.h"
#include "ota_handler.h"  /* OTA 响应字节转发 */
#include "onenet_handler.h" /* ⭐ 阈值下发结果补回服务调用回复 */
#include "gateway_oled.h"  /* ⭐ AUX 超时时 OLED 提示 3s */

#if defined(ESP32)
  #include <HardwareSerial.h>
  static HardwareSerial loraSerial(1);  /* UART1, ESP32 */
#elif LORA_USE_ESP8266_HWSERIAL
  /* ESP8266 模式2: 硬件串口 = UART0 (USB口引脚 RX0/TX0)
   * 此时调试已切到 Serial1 (见 config.h DEBUG_SERIAL) */
  static Stream &loraSerial = Serial;
#else
  #include <SoftwareSerial.h>
  static SoftwareSerial loraSerial(LORA_RX_PIN, LORA_TX_PIN);
#endif

/* ---------- 接收状态机 ----------
 * 从 LoRa 串口读到的字节 (定点传输会自动剥掉 3 字节地址头)
 * 格式: [帧头字节] + payload
 * - LORA_FRAME_DATA: payload = LoraNodeData_t (30字节)
 * - LORA_FRAME_CERT: payload = LoraNodeCert_t
 * - LORA_FRAME_ACK:  payload = ASCII命令字符串 (非固定长度, 以\r结尾) */
enum RxState {
    RX_WAIT_HEADER,      /* 等帧头字节(0xA1/0xB1/0xC1...) */
    RX_FRAME_DATA,       /* 收 LoraNodeData_t */
    RX_FRAME_CERT,       /* 收 LoraNodeCert_t */
    RX_FRAME_ACK         /* 收命令ACK字符串, 读到\r */
};
static RxState  rxState     = RX_WAIT_HEADER;
static uint16_t rxNeed      = 0;   /* 还需收多少字节 */
static uint16_t rxGot       = 0;   /* 已收多少字节 */
static uint32_t lastRxByteMs = 0;  /* 上次收到字节时刻, 状态机超时复位用 */
static uint8_t  rxBuf[sizeof(LoraNodeCert_t) + 1]; /* ⭐ +1: DRSSI附加RSSI字节, 见DX-LR22手册5.3.11 */

/* ---------- 轮询调度 ---------- */
static uint8_t  currentNode   = LORA_POLL_FROM_NODE;   /* 当前处理节点 */
static uint32_t cmdSentAt     = 0;                     /* 命令发出时间 */
static bool     waitingResp   = false;                 /* 是否在等响应 */
static uint32_t respTimeout   = LORA_RESPONSE_TIMEOUT_MS; /* 当前等待的超时值 */

/* 轮询阶段: 0=正常(发DATA/CER), 1=PING已发等PONG, 2=PONG已收等发真实命令 */
static uint8_t  pollPhase     = 0;

/* 搜索模式: 仅开机首次(Flash无证书)或按 FLASH 按钮时置位.
 * 置位期间空槽位(未注册地址)发 PING/CER 发现新节点;
 * 扫完一轮自动恢复 false, 平时只轮询已注册节点 */
static bool     discoveryMode = false;

/* 搜索模式当前地址已 PING 尝试次数 (LoRa 首帧易丢, 超时重试, 总次数见
 * DISCOVER_PING_ATTEMPTS: 3 = 首次 + 2 次重试; 达到上限仍不通则跳过) */
static uint8_t  discoverPingAttempts = 0;

/* 正常轮询周期计时: 本轮(所有节点扫一遍)起始时间戳.
 * 回绕到起始地址时记下, 下一轮必须等满 LORA_POLL_ROUND_MS 才发首条,
 * 防止节点响应快导致连发占满空口 (搜索模式不节流, 见 lora_tick) */
static uint32_t roundStartAt = 0;

/* PING 短超时 (快速探测, 不阻塞) */
#define PING_TIMEOUT_MS         500

/* 证书周期性校验计数: 以 nodeId 为下标, 每 LORA_CERT_VERIFY_EVERY 次
 * 轮询到该节点夹发一次 AT+CER, 用于发现"同地址换节点" */
static uint8_t  dataPollCnt[LORA_MAX_NODES + 1];

/* 离线节点探测退避 (正常轮询对已注册离线节点的 PING 探测):
 * 离线 PING 每超时一次, offlineStage 档位+1(封顶), 下次探测间隔查表
 * OFFLINE_BACKOFF_MS 按指数退避逐档翻倍: 2s→4s→8s→16s→32s→64s;
 * offlineProbeAt = 下次允许探测时刻, 未到则跳过;
 * 收到任何有效帧(数据/证书/PONG)即证明节点存活, 重置档位立即恢复快节奏 */
static const uint32_t OFFLINE_BACKOFF_MS[OFFLINE_BACKOFF_STAGES] =
    { 2000, 4000, 8000, 16000, 32000, 64000 };
static uint8_t  offlineStage[LORA_MAX_NODES + 1];
static uint32_t offlineProbeAt[LORA_MAX_NODES + 1];

/* 返回 true 表示本轮轮到该校验一次证书 */
static bool shouldVerifyCert(uint8_t nodeId)
{
    if (++dataPollCnt[nodeId] >= LORA_CERT_VERIFY_EVERY)
    {
        dataPollCnt[nodeId] = 0;
        return true;
    }
    return false;
}

/* ---------- 控制命令待发队列(简化: 单槽位) ----------
 * 有 OneNET 命令插入时先处理, 下一次 lora_tick 发出去 */
#define PENDING_NONE 0
#define PENDING_SEND 1
static uint8_t  pendingState = PENDING_NONE;
static uint8_t  pendingNode  = 0;
static char     pendingCmd[LORA_CMD_MAX_LEN];

/* ==================== 内部函数 ==================== */

/* 发送定点传输帧: [AddrH][AddrL][CH] + data
 * AUX 忙闲状态判定 (状态码轨迹 [auxBefore->sawHigh->auxAfter]):
 *   [0->1->0] 正常: 发前空闲 -> 发后捕到高(模块收到) -> 等回低(发送完成)
 *   [1->?->?] 发前 AUX 一直高, 模块卡在发送/接收/切换
 *   [0->0->0] 发后 AUX 没变高, 模块未收到数据 (串口/接线异常)
 *   [0->1->1] 发后 AUX 一直高, 模块卡死在发送中
 * 异常时 OLED 显示 "LoRa is OutTime!" 持续 3 秒便于调试
 * 注意: 参考项目每包分开写, 这里也分开避免一次性大缓冲 */
static void sendFixedFrame(uint16_t dstAddr, uint8_t ch,
                           const uint8_t *data, uint16_t len)
{
    /* === 发前: 等 AUX 低(模块空闲), 超时强制发送 === */
    uint32_t t0 = millis();
    while (digitalRead(LORA_AUX_PIN) == HIGH &&
           (millis() - t0) <= LORA_AUX_WAIT_MS) { }
    uint8_t auxBefore = (uint8_t)digitalRead(LORA_AUX_PIN);   /* 期望 0=空闲 */
    uint32_t durBefore = millis() - t0;

    /* === 发数据 === */
    uint8_t header[3];
    header[0] = (uint8_t)(dstAddr >> 8);
    header[1] = (uint8_t)(dstAddr & 0xFF);
    header[2] = ch;
    loraSerial.write(header, 3);
    if (len > 0) loraSerial.write(data, len);

    /* === 发后: 等 AUX 高(模块收到开始处理) -> 等 AUX 低(发送完成) === */
    uint32_t t1 = millis();
    while (digitalRead(LORA_AUX_PIN) == LOW &&
           (millis() - t1) <= LORA_AUX_WAIT_MS) { }   /* 等高 */
    uint8_t sawHigh = (uint8_t)digitalRead(LORA_AUX_PIN);   /* 期望 1=已变高 */
    uint32_t durHigh = millis() - t1;

    uint32_t t2 = millis();
    while (digitalRead(LORA_AUX_PIN) == HIGH &&
           (millis() - t2) <= LORA_AUX_WAIT_MS) { }   /* 等低 */
    uint8_t auxAfter = (uint8_t)digitalRead(LORA_AUX_PIN);   /* 期望 0=完成 */
    uint32_t durLow = millis() - t2;

    /* === 日志输出 ===
     * 正常: 一行 OK, 带节点地址 + 轨迹码 + 总耗时
     * 异常: 一行 FAIL, 带节点地址 + 轨迹码 + 原因 + 各阶段耗时(便于定位) */
    bool ok = (auxBefore == LOW && sawHigh == HIGH && auxAfter == LOW);
    if (ok)
    {
        DBG_PRINTF("[LoRa] TX 0x%04X OK [%d->%d->%d] %lums\n",
                   dstAddr, auxBefore, sawHigh, auxAfter,
                   (unsigned long)(durBefore + durHigh + durLow));
    }
    else
    {
        const char *reason;
        if (auxBefore == HIGH)
            reason = "发前AUX忙, 模块卡在发送/接收/切换";
        else if (sawHigh == LOW)
            reason = "模块未收到数据, 串口/接线异常";
        else if (auxAfter == HIGH)
            reason = "发后AUX一直高, 模块卡死在发送中";
        else
            reason = "未知异常";
        DBG_PRINTF("[LoRa] TX 0x%04X FAIL [%d->%d->%d] %s (前%lu/等高%lu/等低%lu ms)\n",
                   dstAddr, auxBefore, sawHigh, auxAfter, reason,
                   (unsigned long)durBefore,
                   (unsigned long)durHigh,
                   (unsigned long)durLow);
        oled_showTempMessage("LoRa is OutTime!", 3000);
    }
}

/* 把 "AT+<prefix>\r\n" 或 "AT+<prefix>=<value>\r\n" 发到指定节点
 * 节点身份由定点传输帧头 [AddrH][AddrL] 区分, 命令名不携带节点号 */
static void sendAT(uint8_t nodeId, const char *prefix, int value, bool hasValue)
{
    char cmd[LORA_CMD_MAX_LEN];
    int n;
    if (hasValue)
        n = snprintf(cmd, sizeof(cmd), "AT+%s=%d\r\n", prefix, value);
    else
        n = snprintf(cmd, sizeof(cmd), "AT+%s\r\n", prefix);
    if (n <= 0) return;

    sendFixedFrame((uint16_t)nodeId, LORA_CHANNEL, (const uint8_t *)cmd, (uint16_t)n);
    cmdSentAt = millis();
    waitingResp = true;
    DBG_PRINTF("[LoRa] 发送-> 节点%d: %s", nodeId, cmd);
}

/* ⭐ 证书字段 ASCII 校验: ProductKey/DeviceName 必须是可见 ASCII(0x20-0x7E) 且有 \0 结尾
 * 防止 CRC 巧合漏检(1/65536) 的乱码证书被 updateNodeCert 持久化到 LittleFS,
 * 重启后每次加载乱码证书、subLogin 被 ASCII 校验拦截 → 节点永久无法上线 */
static bool certFieldIsAscii(const char *s, size_t maxLen)
{
    if (!s) return false;
    for (size_t i = 0; i < maxLen; i++)
    {
        if (s[i] == '\0') return true;   /* 正常结尾 */
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return false;   /* 走到 maxLen 仍无 \0, 视为非法 */
}

/* ---------- 处理一条完整上行帧 ---------- */
static bool handleCompleteFrame(uint8_t header)
{
    bool gotData = false;

    /* 当前轮询的节点就是响应来源节点 (定点传输已按地址区分,
     * 这里直接认为是 currentNode) */
    uint8_t nodeId = currentNode;

    switch (header)
    {
    case LORA_FRAME_DATA:
        if (rxGot != sizeof(LoraNodeData_t) + 1)   /* ⭐ +1: 末字节为DRSSI附加RSSI */
        {
            DBG_PRINTF("[LoRa] 数据长度不匹配 (%u vs %u)\n",
                       (unsigned)rxGot, (unsigned)(sizeof(LoraNodeData_t) + 1));
            break;
        }
        /* ⭐ v2 协议: CRC16 校验, 不计末字节RSSI, 防止链路错位/噪声/状态机
         * 残留被解析成"合法帧"导致垃圾数据上报到 OneNET 平台 */
        {
            /* 剥离末字节RSSI: 换算公式 -(0xFF - byte), 见DX-LR22手册5.3.11 */
            int8_t rssi = (int8_t)(0 - (int)(0xFF - (uint8_t)rxBuf[rxGot - 1]));
            LoraNodeData_t *d = (LoraNodeData_t *)rxBuf;
            uint16_t calc = lora_crc16(rxBuf, offsetof(LoraNodeData_t, crc16));
            if (calc != d->crc16)
            {
                DBG_PRINTF("[LoRa] 数据帧 CRC 错 (节点%d seq=%d 算=%04X 收=%04X) → 丢弃\n",
                           nodeId, d->seq, calc, d->crc16);
                break;   /* CRC 错: 不调 updateNodeFromRaw, 直接丢 */
            }
            /* ⭐ 双保险: 字段合理性校验 (即便 CRC 通过, 也挡巧合值) */
            if (d->ParkStatus > 2 || d->Ultrasonic > 1000 || d->OccupiedTime > 86400)
            {
                DBG_PRINTF("[LoRa] 数据帧字段越界 (节点%d ParkStatus=%d 距离=%d 时长=%lu) → 丢弃\n",
                           nodeId, d->ParkStatus, d->Ultrasonic,
                           (unsigned long)d->OccupiedTime);
                break;
            }
            updateNodeFromRaw(nodeId, d);
            /* ⭐ RSSI 存入对应节点, 供 MQTT 代子设备上报 */
            int slot = findNode(nodeId);
            if (slot >= 0)
                nodes[slot].rssi = rssi;
            gotData = true;
            DBG_PRINTF("[LoRa] 收到<- 节点%d 数据: 车位=%d 距离=%d 地磁=%d 时长=%lu LED=%d 使能=%d (seq=%d) RSSI=%ddBm\n",
                       nodeId, d->ParkStatus, d->Ultrasonic,
                       d->GeoMagnetic, (unsigned long)d->OccupiedTime,
                       d->LED, d->LedEnable, d->seq, rssi);
        }
        break;

    case LORA_FRAME_CERT:
        if (rxGot != sizeof(LoraNodeCert_t) + 1)   /* ⭐ +1: 末字节为DRSSI附加RSSI */
        {
            DBG_PRINTF("[LoRa] 证书长度不匹配 (%u vs %u)\n",
                       (unsigned)rxGot, (unsigned)(sizeof(LoraNodeCert_t) + 1));
            break;
        }
        {
            const LoraNodeCert_t *cert = (const LoraNodeCert_t *)rxBuf;
            /* ⭐ v2 协议: CRC16 校验, 防止证书字节流错位导致 productKey/deviceName
             * 是乱码仍触发代上线请求, OneNET 平台返回 code=2402 request format error */
            uint16_t calc = lora_crc16(rxBuf, offsetof(LoraNodeCert_t, crc16));
            if (calc != cert->crc16)
            {
                DBG_PRINTF("[LoRa] 证书帧 CRC 错 (节点%d seq=%d 算=%04X 收=%04X) → 丢弃\n",
                           nodeId, cert->seq, calc, cert->crc16);
                break;
            }
            /* ⭐ 第三层防御: CRC 通过后再校验 ProductKey/DeviceName 是可见 ASCII,
             * 防止 CRC 巧合漏检(1/65536)的乱码证书被持久化到 LittleFS,
             * 重启后加载乱码证书导致 subLogin 永久被拦截、节点无法上线 */
            if (!certFieldIsAscii(cert->ProductKey, sizeof(cert->ProductKey)) ||
                !certFieldIsAscii(cert->DeviceName, sizeof(cert->DeviceName)))
            {
                DBG_PRINTF("[LoRa] 证书帧字段非可见 ASCII (节点%d) → 丢弃, 不持久化\n",
                           nodeId);
                break;
            }
            updateNodeCert(nodeId, cert);
            DBG_PRINTF("[LoRa] 收到<- 节点%d 证书 (有效=%d 产品=%s 设备=%s seq=%d)\n",
                       nodeId, cert->valid, cert->ProductKey, cert->DeviceName, cert->seq);
        }
        gotData = true;
        break;

    case LORA_FRAME_ACK:
        rxBuf[rxGot] = '\0';
        DBG_PRINTF("[LoRa] 收到<- 节点%d 确认: %s\n", nodeId, (char *)rxBuf);
        gotData = true;
        /* ⭐ 阈值下发成功: 收到 ACK 后才清除标志, 重置重试计数,
         * 并向平台补回"同步服务调用"回复(成功 Result=1) */
        if (strcmp((char *)rxBuf, "AT+ZombieThreshold") == 0)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
            {
                nodes[slot].thresholdNeedsUpdate = false;
                nodes[slot].thresholdRetryCount = 0;
                DBG_PRINTF("[LoRa] 节点%d 僵尸车阈值下发成功\n", nodeId);
                onenet_notifyServiceResult(slot, true, nodes[slot].thresholdValue);
            }
        }
        /* ⭐ LED 控制下发成功: 收到 ACK 后向平台补回"同步服务调用"回复 */
        if (strcmp((char *)rxBuf, "AT+LedEnable") == 0)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
                onenet_notifyServiceResult(slot, true, 1);
        }
        /* PING 通(PONG)即视为节点存活: 刷新在线状态与活性时间,
         * 并处理"掉线恢复"需要重新代上线(与收到数据的恢复逻辑一致) */
        if (strcmp((char *)rxBuf, "PONG") == 0)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
            {
                NodeData &nd = nodes[slot];
                bool wasOffline = !nd.online;
                nd.online     = true;
                nd.lastUpdate = millis();
                if (wasOffline && nd.certSent && !nd.subLogin)
                    nd.loginPending = true;
                /* ⭐ 方案三: PONG 确认在线后, 触发阈值下发 (限次重试)
                 * 最多重试 3 次, 超过后放弃 (防止旧固件节点阻塞) */
                if (nd.thresholdNeedsUpdate && nd.certSent && nd.thresholdRetryCount < 3)
                {
                    nd.thresholdRetryCount++;  /* 重试次数+1 */
                    lora_sendControl(nodeId, "ZombieThreshold", nd.thresholdValue);
                    if (nd.thresholdRetryCount >= 3)
                    {
                        /* 超过3次, 清除标志, 等待用户重新下发或节点升级后重试 */
                        nd.thresholdNeedsUpdate = false;
                        onenet_notifyServiceResult(slot, false, 0);   /* ⭐ 回平台失败 */
                        DBG_PRINTF("[LoRa] 节点%d 僵尸车阈值重试%d次失败, 放弃 (等节点升级或重新下发)\n",
                                   nodeId, nd.thresholdRetryCount);
                    }
                    else
                    {
                        DBG_PRINTF("[LoRa] 节点%d 在线, 下发僵尸车阈值=%d秒 (重试%d/3, 待ACK)\n",
                                   nodeId, nd.thresholdValue, nd.thresholdRetryCount);
                    }
                }
            }
        }
        break;

    default:
        DBG_PRINTF("[LoRa] 未知帧头 0x%02X\n", header);
        break;
    }

    /* 收到任何有效帧即确认节点存活: 重置离线探测退避,
     * 使其立即恢复正常快节奏(否则离线档位会一直压制探测间隔) */
    if (gotData && nodeId <= LORA_MAX_NODES)
    {
        offlineStage[nodeId]   = 0;
        offlineProbeAt[nodeId] = 0;
    }
    return gotData;
}

/* ---------- 把串口字节扔进接收状态机 ---------- */
static bool feedRx(uint8_t c)
{
    bool gotFrame = false;

    switch (rxState)
    {
    case RX_WAIT_HEADER:
        /* OTA 响应字节 (ACK/NAK/CAN): 直接转发给 OTA 处理器, 不进入帧状态机 */
        if (c == OTA_ACK || c == OTA_NAK || c == OTA_CAN)
        {
            ota_feedByte(c);
            break;
        }
        /* ⭐ 严格帧头白名单: 只接受 5 个合法帧头字节, 其他字节直接丢弃
         * 防止 AT 命令回执/串口噪声/状态机错位被误识别为帧头 */
        if (c == LORA_FRAME_CERT || c == LORA_FRAME_DATA || c == LORA_FRAME_ACK
         || c == LORA_FRAME_OTA_OK || c == LORA_FRAME_OTA_RETRY)
        {
            rxState = (c == LORA_FRAME_CERT) ? RX_FRAME_CERT
                   : (c == LORA_FRAME_DATA) ? RX_FRAME_DATA
                   :                          RX_FRAME_ACK;
            /* ⭐ DRSSI: 接收端模块开启数据包RSSI后, 收包末尾会被附加1字节
             * 实时RSSI(见DX-LR22手册5.3.11). 故 DATA/CERT 都多收1字节,
             * 解析时最后一字节作RSSI剥离, 不参与CRC/字段校验. ACK以\r结尾
             * 不受影响(附加字节在\r后, 会作为垃圾帧头被丢弃). */
            rxNeed  = (rxState == RX_FRAME_CERT) ? (uint16_t)(sizeof(LoraNodeCert_t) + 1)
                   : (rxState == RX_FRAME_DATA) ? (uint16_t)(sizeof(LoraNodeData_t) + 1)
                   :                              (uint16_t)(sizeof(rxBuf) - 1);
            rxGot   = 0;
            /* ⭐ v2 加固: 进入新状态时清零 rxBuf, 防止上次残留字节污染本次解析
             * 历史乱码 bug 根因之一: rxBuf 上次未清零, 凑齐长度后解析出垃圾 */
            memset(rxBuf, 0, rxNeed);
            lastRxByteMs = millis();
            /* 把帧头字节保留, 供 handleCompleteFrame 读取:
             * 我们不存到rxBuf里, 而是通过函数参数传header */
            (void)c;
        }
        /* ⭐ 非合法帧头字节: 直接 break 丢弃, 状态保持 RX_WAIT_HEADER 等下个字节 */
        break;

    case RX_FRAME_ACK:
        /* ACK 字符串, 以 \r 结尾 (节点端命令行协议以 \r\n 结尾) */
        lastRxByteMs = millis();
        if (c == '\r' || rxGot >= rxNeed - 1)
        {
            rxBuf[rxGot] = '\0';
            gotFrame = handleCompleteFrame(LORA_FRAME_ACK);
            rxState  = RX_WAIT_HEADER;
        }
        else if (c != '\n')   /* \n 忽略 */
        {
            rxBuf[rxGot++] = c;
        }
        break;

    case RX_FRAME_DATA:
    case RX_FRAME_CERT:
        lastRxByteMs = millis();
        rxBuf[rxGot++] = c;
        if (rxGot >= rxNeed)
        {
            uint8_t hdr = (rxState == RX_FRAME_DATA) ? LORA_FRAME_DATA : LORA_FRAME_CERT;
            gotFrame = handleCompleteFrame(hdr);
            rxState  = RX_WAIT_HEADER;
        }
        break;
    }
    return gotFrame;
}

/* ---------- 选下一个要轮询的节点 ---------- */
static void advanceNextNode(void)
{
    discoverPingAttempts = 0;   /* 换地址时重置搜索 PING 尝试计数 */
    pollPhase       = 0;   /* 换地址时复位轮询阶段, 防止 PONG 已收(阶段2)
                            * 被下一个节点继承, 跳过 PING 直接发 CER/DATA */
    currentNode++;
    if (currentNode > LORA_POLL_TO_NODE)
    {
        currentNode = LORA_POLL_FROM_NODE;
        roundStartAt = millis();   /* 新一轮开始计时 (正常轮询节流) */
        /* 一轮地址扫完: 自动退出搜索模式, 恢复只轮询已注册节点 */
        if (discoveryMode)
        {
            discoveryMode = false;
            DBG_PRINTLN("[LoRa] 节点发现扫描完成, 恢复正常轮询");
        }
    }
}

/* ==================== 公开函数 ==================== */

void lora_init(void)
{
    /* AUX 输入: 模块忙闲状态. M0/M1 直连 GND 不占 GPIO */
    pinMode(LORA_AUX_PIN, INPUT);

#if defined(ESP32)
    loraSerial.begin(LORA_BAUD, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
#else
    loraSerial.begin(LORA_BAUD);
#endif
    rxState = RX_WAIT_HEADER;
    currentNode = LORA_POLL_FROM_NODE;
    waitingResp = false;
    pollPhase   = 0;
    roundStartAt = 0;   /* 启动即视为新一轮开始, 首轮立即轮询 */
    DBG_PRINTF("[LoRa] 串口就绪 (波特率=%d, 硬串=%d, 网关=0x%04X, 信道=%d)\n",
               LORA_BAUD, LORA_USE_HWSERIAL, LORA_GATEWAY_ADDR, LORA_CHANNEL);
}

bool lora_tick(void)
{
    bool gotData = false;
    uint32_t now = millis();

    /* --- 0. 状态机超时复位: 某次进入 RX_FRAME_DATA/CERT/ACK 后未收齐
     * (节点发了短帧/丢包/串口中断), 状态机卡死, 下次 0xB1 帧头会被
     * 当作数据字节污染 rxBuf → 凑齐长度后解析出垃圾 → 乱码上线请求.
     * 500ms 未收齐强制回 RX_WAIT_HEADER + rxGot=0 + rxBuf 清零 */
    if (rxState != RX_WAIT_HEADER && (now - lastRxByteMs) > 500)
    {
        DBG_PRINTF("[LoRa] RX 状态机超时 (state=%d, %lums), 强制复位\n",
                   (int)rxState, (unsigned long)(now - lastRxByteMs));
        rxState = RX_WAIT_HEADER;
        rxGot   = 0;
        memset(rxBuf, 0, sizeof(rxBuf));
    }

    /* --- 1. 先把串口数据吃干净 --- */
    while (loraSerial.available())
    {
        if (feedRx((uint8_t)loraSerial.read()))
        {
            gotData = true;
            if (waitingResp)
            {
                waitingResp = false;   /* 收到响应, 本轮结束 */
                if (pollPhase == 1)
                {
                    /* PING 收到回复(PONG) → 切到阶段2, 不发前移,
                     * 下一轮 section 4 发真实命令(CER/DATA) */
                    pollPhase = 2;
                }
                else
                {
                    /* 真实命令的回复(CER/DATA/ACK) → 正常前移 */
                    pollPhase = 0;
                    advanceNextNode();
                }
            }
        }
    }

    /* --- 1.5 OTA 进行中: 只收字节(喂 OTA 响应), 暂停一切轮询/控制命令
     * 发送. 节点正复位进 BootLoader, 不会响应 AT 命令, 此时继续发命令
     * 只会与 OTA 数据包争抢半双工空口, 干扰升级; OTA 结束后自然恢复.
     * 注意: 不能整体跳过本函数, 否则 OTA 的 ACK/NAK 响应也收不到了 */
    if (ota_getState() != OTA_IDLE)
    {
        return gotData;
    }

    /* --- 2. 处理待发控制命令 (优先于轮询, 且不等待响应不算节点轮询) --- */
    if (pendingState == PENDING_SEND && !waitingResp)
    {
        sendFixedFrame((uint16_t)pendingNode, LORA_CHANNEL,
                       (const uint8_t *)pendingCmd, strlen(pendingCmd));
        pendingState = PENDING_NONE;
        cmdSentAt = now;
        waitingResp = true;
        respTimeout = LORA_RESPONSE_TIMEOUT_MS;  /* 控制命令用正常超时 */
        pollPhase   = 0;                         /* 控制命令不参与 PING 阶段 */
        DBG_PRINTF("[LoRa] 下发控制: %s", pendingCmd);
        return gotData;
    }

    /* --- 3. 如果还在等响应, 判断超时 --- */
    if (waitingResp)
    {
        if (now - cmdSentAt > respTimeout)
        {
            /* 搜索模式 PING 超时: LoRa 首帧易丢, 同地址重试再放弃;
             * 尝试计数未达总次数上限(含首次, 见 DISCOVER_PING_ATTEMPTS)就重发;
             * 非搜索模式(掉线恢复探测)保持单次, 下一轮再查 */
            if (discoveryMode && pollPhase == 1 &&
                discoverPingAttempts < DISCOVER_PING_ATTEMPTS - 1)
            {
                discoverPingAttempts++;
                char cmd[LORA_CMD_MAX_LEN];
                int n = snprintf(cmd, sizeof(cmd), "AT+PING\r\n");
                if (n > 0)
                    sendFixedFrame((uint16_t)currentNode, LORA_CHANNEL,
                                   (const uint8_t *)cmd, (uint16_t)n);
                (void)n;
                cmdSentAt = now;
                waitingResp = true;
                respTimeout = PING_TIMEOUT_MS;
                pollPhase   = 1;
                DBG_PRINTF("[LoRa] PING-> 节点%d (尝试%d/%d)\n",
                           currentNode, discoverPingAttempts, DISCOVER_PING_ATTEMPTS);
                return gotData;
            }
            DBG_PRINTF("[LoRa] 节点%d 超时 (跳过)\n", currentNode);
            /* 正常模式离线节点 PING 超时: 离线探测退避档位+1, 拉长下次
             * 探测间隔; 搜索模式 / 在线节点 DATA 超时不做退避 */
            if (!discoveryMode && pollPhase == 1)
            {
                if (offlineStage[currentNode] < OFFLINE_BACKOFF_STAGES - 1)
                    offlineStage[currentNode]++;
                offlineProbeAt[currentNode] =
                    millis() + OFFLINE_BACKOFF_MS[offlineStage[currentNode]];
            }
            waitingResp = false;
            pollPhase   = 0;     /* 超时 → 重置阶段, 正常前移 */
            discoverPingAttempts = 0;
            advanceNextNode();
        }
        return gotData;   /* 等当前响应, 暂不发下一条 */
    }

    /* --- 4. 发下一条轮询命令 --- */
    {
        /* 正常轮询节流: 本轮(回绕起算)未满 LORA_POLL_ROUND_MS 就不发下一条,
         * 防止节点响应快导致连发占满 LoRa 空口(半双工共享信道易撞包).
         * 搜索模式不节流(探测节奏由 PING 超时自然控制);
         * pollPhase==2 是 PING 流程延续(PONG 已收待发真实命令), 也不拦 */
        if (!discoveryMode && pollPhase == 0 &&
            (now - roundStartAt < LORA_POLL_ROUND_MS))
        {
            return gotData;
        }

        int slot = findNode(currentNode);
        bool certOk = (slot >= 0) && (nodes[slot].certSent);
        bool online = (slot >= 0) && (nodes[slot].online);

        if (pollPhase == 2)
        {
            /* PONG 已收, 发真实命令:
             *   搜索模式: 无证书 → CER 注册; 有证书 → PING 已确认在线, 直接跳过
             *   正常模式: 未注册 → CER; 已注册 → DATA (或每50次夹发CER校验) */
            if (discoveryMode)
            {
                if (!certOk)
                {
                    sendAT(currentNode, "CER", 0, false);
                    respTimeout = LORA_RESPONSE_TIMEOUT_MS;
                    pollPhase   = 0;
                }
                else
                    advanceNextNode();   /* 有证书: 探测在线即可, 无需再要证书 */
            }
            else
            {
                bool wantCert = !certOk ||
                                (certOk && shouldVerifyCert(currentNode));
                sendAT(currentNode, wantCert ? "CER" : "DATA", 0, false);
                respTimeout = LORA_RESPONSE_TIMEOUT_MS;
                pollPhase   = 0;
            }
        }
        else if (discoveryMode)
        {
            /* 搜索模式: 对每个节点(无论是否已注册)先 PING 探测,
             * 通了下一轮发 CER 要证书; 不通(500ms 超时)则换下一个 */
            char cmd[LORA_CMD_MAX_LEN];
            int n = snprintf(cmd, sizeof(cmd), "AT+PING\r\n");
            if (n > 0)
                sendFixedFrame((uint16_t)currentNode, LORA_CHANNEL,
                               (const uint8_t *)cmd, (uint16_t)n);
            (void)n;
            cmdSentAt = now;
            waitingResp = true;
            respTimeout = PING_TIMEOUT_MS;
            pollPhase   = 1;
            DBG_PRINTF("[LoRa] PING-> 节点%d\n", currentNode);
        }
        else if (certOk && online)
        {
            /* 已注册在线节点: 快速路径, 直接发 DATA (不经过 PING) */
            /* ⭐ 方案三补充: 若该节点有待下发的阈值, 且重试次数<3, 先下发阈值 */
            if (slot >= 0 && nodes[slot].thresholdNeedsUpdate && nodes[slot].thresholdRetryCount < 3)
            {
                nodes[slot].thresholdRetryCount++;  /* 重试次数+1 */
                lora_sendControl(currentNode, "ZombieThreshold", nodes[slot].thresholdValue);
                if (nodes[slot].thresholdRetryCount >= 3)
                {
                    /* 超过3次, 清除标志, 防止阻塞 */
                    nodes[slot].thresholdNeedsUpdate = false;
                    onenet_notifyServiceResult(slot, false, 0);   /* ⭐ 回平台失败 */
                    DBG_PRINTF("[LoRa] 节点%d 僵尸车阈值重试%d次失败, 放弃 (等节点升级或重新下发)\n",
                               currentNode, nodes[slot].thresholdRetryCount);
                }
                else
                {
                    DBG_PRINTF("[LoRa] 节点%d 在线, 快速路径下发僵尸车阈值=%d秒 (重试%d/3, 待ACK)\n",
                               currentNode, nodes[slot].thresholdValue, nodes[slot].thresholdRetryCount);
                }
            }
            else
            {
                bool verify = shouldVerifyCert(currentNode);
                sendAT(currentNode, verify ? "CER" : "DATA", 0, false);
                respTimeout = LORA_RESPONSE_TIMEOUT_MS;
            }
        }
        else if (slot < 0)
        {
            /* 非搜索模式: 空槽位(未注册地址)直接跳过, 不打扰;
             * 只在开机首次或按 FLASH 按钮触发发现时才扫描 */
            advanceNextNode();
        }
        else
        {
            /* 正常模式离线节点: 先 PING 确认在线;
             * 离线退避: 未到下次探测时刻(offlineProbeAt)直接跳过,
             * 离线越久探测间隔越稀疏, 避免长期离线节点拖慢轮询一圈 */
            if (now < offlineProbeAt[currentNode])
            {
                advanceNextNode();
                return gotData;
            }
            char cmd[LORA_CMD_MAX_LEN];
            int n = snprintf(cmd, sizeof(cmd), "AT+PING\r\n");
            if (n > 0)
                sendFixedFrame((uint16_t)currentNode, LORA_CHANNEL,
                               (const uint8_t *)cmd, (uint16_t)n);
            (void)n;
            cmdSentAt = now;
            waitingResp = true;
            respTimeout = PING_TIMEOUT_MS;
            pollPhase   = 1;
            DBG_PRINTF("[LoRa] PING-> 节点%d\n", currentNode);
        }
    }

    return gotData;
}

/* 手动触发节点发现: 从头扫描所有节点 (进入搜索模式, 扫完一轮自动退出) */
void lora_triggerDiscovery(void)
{
    currentNode = LORA_POLL_FROM_NODE;
    waitingResp = false;
    pollPhase   = 0;
    discoverPingAttempts = 0;   /* 重新触发扫描时清零尝试计数 */
    discoveryMode = true;
    DBG_PRINTLN("[LoRa] 手动触发节点发现 (全量扫描)");
}

bool lora_discoveryActive(void)
{
    return discoveryMode;
}

void lora_sendControl(uint8_t nodeId, const char *property, int value)
{
    /* 组装 AT+<property>=<value>\r\n
     * 节点端回调按纯名称匹配 (如 "AT+LedEnable"), 节点地址靠定点传输[AddrH][AddrL]区分,
     * 命令名不包含 nodeId */
    int n = snprintf(pendingCmd, sizeof(pendingCmd), "AT+%s=%d\r\n", property, value);
    (void)n;
    pendingNode  = nodeId;
    pendingState = PENDING_SEND;
}

Stream &lora_getSerial(void) { return loraSerial; }
