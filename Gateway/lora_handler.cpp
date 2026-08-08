/* lora_handler.cpp - Gateway LoRa 定点传输 + 轮询调度
 *
 * 数据流向:
 *   发送 (下行): [AddrH][AddrL][CH] + "AT+XXX<n>[=v]\r\n"
 *   接收 (上行): 定点模块剥掉帧头 -> [帧头字节] + [二进制结构体]
 *
 * 轮询策略:
 *   1. 先做"证书发现": AT+CER1~CERn (找到 certSent=true 就算注册)
 *   2. 已注册节点: 周期性 AT+DATAn (收集数据)
 *   3. 单命令最大等待 LORA_RESPONSE_TIMEOUT_MS, 超时跳过下一节点
 */
#include "lora_handler.h"
#include "config.h"
#include "node_data.h"

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

/* 把 "AT+<prefix><nodeId>\r\n" 或 "AT+<prefix><nodeId>=<value>\r\n"
 * 发到指定节点 */
static void sendAT(uint8_t nodeId, const char *prefix, int value, bool hasValue)
{
    char cmd[LORA_CMD_MAX_LEN];
    int n;
    if (hasValue)
        n = snprintf(cmd, sizeof(cmd), "AT+%s%d=%d\r\n", prefix, nodeId, value);
    else
        n = snprintf(cmd, sizeof(cmd), "AT+%s%d\r\n", prefix, nodeId);
    if (n <= 0) return;

    sendFixedFrame((uint16_t)nodeId, LORA_CHANNEL, (const uint8_t *)cmd, (uint16_t)n);
    cmdSentAt = millis();
    waitingResp = true;
    DBG_PRINTF("[LoRa] -> node%d: %s", nodeId, cmd);
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
            DBG_PRINTF("[LoRa] DATA size mismatch (%u vs %u)\n",
                       (unsigned)rxGot, (unsigned)sizeof(LoraNodeData_t));
            break;
        }
        updateNodeFromRaw(nodeId, (const LoraNodeData_t *)rxBuf);
        gotData = true;
        {
            LoraNodeData_t *d = (LoraNodeData_t *)rxBuf;
            DBG_PRINTF("[LoRa] <- node%d DATA: ps=%d us=%d gm=%d ot=%lu led=%d le=%d\n",
                       nodeId, d->ParkStatus, d->Ultrasonic,
                       d->GeoMagnetic, (unsigned long)d->OccupiedTime,
                       d->LED, d->LedEnable);
            (void)d;
        }
        break;

    case LORA_FRAME_CERT:
        if (rxGot != sizeof(LoraNodeCert_t))
        {
            DBG_PRINTF("[LoRa] CERT size mismatch (%u vs %u)\n",
                       (unsigned)rxGot, (unsigned)sizeof(LoraNodeCert_t));
            break;
        }
        {
            const LoraNodeCert_t *cert = (const LoraNodeCert_t *)rxBuf;
            updateNodeCert(nodeId, cert);
            DBG_PRINTF("[LoRa] <- node%d CERT (valid=%d pk=%s dn=%s)\n",
                       nodeId, cert->valid, cert->ProductKey, cert->DeviceName);
        }
        gotData = true;
        break;

    case LORA_FRAME_ACK:
        rxBuf[rxGot] = '\0';
        DBG_PRINTF("[LoRa] <- node%d ACK: %s\n", nodeId, (char *)rxBuf);
        gotData = true;
        break;

    default:
        DBG_PRINTF("[LoRa] unknown frame header 0x%02X\n", header);
        break;
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
    currentNode++;
    if (currentNode > LORA_POLL_TO_NODE)
        currentNode = LORA_POLL_FROM_NODE;
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
    DBG_PRINTF("[LoRa] Serial ready (baud=%d, hw=%d, gw=0x%04X, ch=%d)\n",
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
                waitingResp = false;   /* 收到响应, 本轮结束 */
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
        DBG_PRINTF("[LoRa] -> control: %s", pendingCmd);
        return gotData;
    }

    /* --- 3. 如果还在等响应, 判断超时 --- */
    if (waitingResp)
    {
        if (now - cmdSentAt > LORA_RESPONSE_TIMEOUT_MS)
        {
            DBG_PRINTF("[LoRa] node%d timeout (skip)\n", currentNode);
            waitingResp = false;
            advanceNextNode();
        }
        return gotData;   /* 等当前响应, 暂不发下一条 */
    }

    /* --- 4. 发下一条轮询命令 --- */
    {
        int slot = findNode(currentNode);
        bool certOk = (slot >= 0) && (nodes[slot].certSent);
        /* 未证书: AT+CERx, 已注册: AT+DATAx */
        sendAT(currentNode, certOk ? "DATA" : "CER", 0, false);
    }

    return gotData;
}

void lora_sendControl(uint8_t nodeId, const char *property, int value)
{
    /* 组装 AT+<property><nodeId>=<value>\r\n  (注意原节点端匹配 "AT+<property>") */
    int n = snprintf(pendingCmd, sizeof(pendingCmd), "AT+%s=%d\r\n", property, value);
    /* 说明: 节点端回调目前只识别 property=="LedEnable" 等纯名称匹配,
     *       节点地址靠定点传输[AddrH][AddrL]区分, 命令名不包含nodeId */
    (void)n;
    pendingNode  = nodeId;
    pendingState = PENDING_SEND;
}

Stream &lora_getSerial(void) { return loraSerial; }
