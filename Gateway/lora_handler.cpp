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
 * - LORA_FRAME_DATA: payload = LoraNodeData_t (10字节)
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
static uint8_t  rxBuf[sizeof(LoraNodeCert_t)]; /* 最大结构体大小够放 Cert */

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
 * 注意: 参考项目每包分开写, 这里也分开避免一次性大缓冲 */
static void sendFixedFrame(uint16_t dstAddr, uint8_t ch,
                           const uint8_t *data, uint16_t len)
{
    uint8_t header[3];
    header[0] = (uint8_t)(dstAddr >> 8);
    header[1] = (uint8_t)(dstAddr & 0xFF);
    header[2] = ch;
    loraSerial.write(header, 3);
    if (len > 0) loraSerial.write(data, len);
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
        if (rxGot != sizeof(LoraNodeData_t))
        {
            DBG_PRINTF("[LoRa] 数据长度不匹配 (%u vs %u)\n",
                       (unsigned)rxGot, (unsigned)sizeof(LoraNodeData_t));
            break;
        }
        updateNodeFromRaw(nodeId, (const LoraNodeData_t *)rxBuf);
        gotData = true;
        {
            LoraNodeData_t *d = (LoraNodeData_t *)rxBuf;
            DBG_PRINTF("[LoRa] 收到<- 节点%d 数据: 车位=%d 距离=%d 地磁=%d 时长=%lu LED=%d 使能=%d\n",
                       nodeId, d->ParkStatus, d->Ultrasonic,
                       d->GeoMagnetic, (unsigned long)d->OccupiedTime,
                       d->LED, d->LedEnable);
            (void)d;
        }
        break;

    case LORA_FRAME_CERT:
        if (rxGot != sizeof(LoraNodeCert_t))
        {
            DBG_PRINTF("[LoRa] 证书长度不匹配 (%u vs %u)\n",
                       (unsigned)rxGot, (unsigned)sizeof(LoraNodeCert_t));
            break;
        }
        {
            const LoraNodeCert_t *cert = (const LoraNodeCert_t *)rxBuf;
            updateNodeCert(nodeId, cert);
            DBG_PRINTF("[LoRa] 收到<- 节点%d 证书 (有效=%d 产品=%s 设备=%s)\n",
                       nodeId, cert->valid, cert->ProductKey, cert->DeviceName);
        }
        gotData = true;
        break;

    case LORA_FRAME_ACK:
        rxBuf[rxGot] = '\0';
        DBG_PRINTF("[LoRa] 收到<- 节点%d 确认: %s\n", nodeId, (char *)rxBuf);
        gotData = true;
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
        if (c == LORA_FRAME_CERT || c == LORA_FRAME_DATA || c == LORA_FRAME_ACK
         || c == LORA_FRAME_OTA_OK || c == LORA_FRAME_OTA_RETRY)
        {
            rxState = (c == LORA_FRAME_CERT) ? RX_FRAME_CERT
                   : (c == LORA_FRAME_DATA) ? RX_FRAME_DATA
                   :                          RX_FRAME_ACK;
            rxNeed  = (rxState == RX_FRAME_CERT) ? (uint16_t)sizeof(LoraNodeCert_t)
                   : (rxState == RX_FRAME_DATA) ? (uint16_t)sizeof(LoraNodeData_t)
                   :                              (uint16_t)(sizeof(rxBuf) - 1);
            rxGot   = 0;
            /* 把帧头字节保留, 供 handleCompleteFrame 读取:
             * 我们不存到rxBuf里, 而是通过函数参数传header */
            (void)c;
        }
        break;

    case RX_FRAME_ACK:
        /* ACK 字符串, 以 \r 结尾 (节点端命令行协议以 \r\n 结尾) */
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
            bool verify = shouldVerifyCert(currentNode);
            sendAT(currentNode, verify ? "CER" : "DATA", 0, false);
            respTimeout = LORA_RESPONSE_TIMEOUT_MS;
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
