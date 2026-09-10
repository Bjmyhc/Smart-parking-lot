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
/* ⭐ 状态机状态名: 超时日志打印语义名(替代裸数字), 便于现场判读 */
static const char *rxStateName(RxState s)
{
    switch (s)
    {
        case RX_WAIT_HEADER: return "RX_WAIT_HEADER";
        case RX_FRAME_DATA:  return "RX_FRAME_DATA";
        case RX_FRAME_CERT:  return "RX_FRAME_CERT";
        case RX_FRAME_ACK:   return "RX_FRAME_ACK";
        default:             return "?";
    }
}
/* ⭐ S17: 接收缓冲固定 64B, 不再依赖结构体大小"恰好装下" (方案 8.4.3):
 *   - DATA/CERT 帧: 需 sizeof(结构体)+1 (末字节DRSSI), 解析前有显式长度校验
 *   - ACK 帧: 命令回显字符串, 以 \r 结尾, 上限 RX_BUF_SIZE-1
 * 编译期断言固化边界: 两结构体+RSSI 必须能装下, 防未来字段扩张静默溢出 */
#define RX_BUF_SIZE 64
static uint8_t  rxBuf[RX_BUF_SIZE];
static_assert(sizeof(LoraNodeData_t) + 1 <= RX_BUF_SIZE, "LoraNodeData_t+RSSI 超出 RX_BUF_SIZE");
static_assert(sizeof(LoraNodeCert_t) + 1 <= RX_BUF_SIZE, "LoraNodeCert_t+RSSI 超出 RX_BUF_SIZE");

/* ---------- 轮询调度 ---------- */
static uint8_t  currentNode   = LORA_POLL_FROM_NODE;   /* 当前处理节点 */
static uint32_t cmdSentAt     = 0;                     /* 命令发出时间 */
static bool     waitingResp   = false;                 /* 是否在等响应 */
static uint32_t respTimeout   = LORA_RESPONSE_TIMEOUT_MS; /* 当前等待的超时值 */
/* ⭐ 快速路径(DATA/CER)正在等待响应标志: 仅 APP 在线节点快速路径置位.
 * 超时时据此递增 dataMissCount, 达 DATA_MISS_OFFLINE_MAX 次无响应自动
 * 降级为 PING 探测, 消灭"在线僵尸"通道 (方案 7.1#3) */
static bool     waitingFastPath = false;

/* 轮询阶段: 0=正常(发DATA/CER), 1=PING已发等PONG, 2=PONG已收等发真实命令 */
static uint8_t  pollPhase     = 0;

/* 搜索模式: 仅开机首次(Flash无证书)或按 FLASH 按钮时置位.
 * 置位期间空槽位(未注册地址)发 PING/CER 发现新节点;
 * 扫完一轮自动恢复 false, 平时只轮询已注册节点 */
static bool     discoveryMode = false;

/* 搜索模式当前地址已 PING 尝试次数 (LoRa 首帧易丢, 超时重试, 总次数见
 * DISCOVER_PING_ATTEMPTS: 3 = 首次 + 2 次重试; 达到上限仍不通则跳过) */
static uint8_t  discoverPingAttempts = 0;

/* ⭐ S35: 队列控制命令待回执标志. 命令回执只清等待、不推进轮询节点,
 * 否则节点1 的 2 条阈值命令回执各消耗一次 advanceNextNode, 把搜索模式
 * 的下一地址(节点2)跳过后误判"一轮扫完"提前退出, 新节点永远发现不到 */
static bool     waitingCmdReply = false;        /* 正在等队列命令回执 */
static uint8_t  savedPollPhaseBeforeCmd = 0;    /* 投递命令前的轮询阶段(PONG后=2) */

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

/* ⭐ 链路统计: 每 60s 汇总打印一行, 长跑时一眼看出空口质量, 无需翻整段
 * 日志. TX失败=AUX轨迹异常, 响应超时/状态机复位/ACK丢弃 分别对应节点
 * 无回复、半帧污染、迟到或杂散 ACK */
#define STAT_REPORT_MS          60000UL
static uint32_t statTxOk        = 0;
static uint32_t statTxFail      = 0;
static uint32_t statRespTimeout = 0;
static uint32_t statRxReset     = 0;
static uint32_t statAckDrop     = 0;
static uint32_t statLastReport  = 0;

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

/* ---------- 控制命令环形队列 (S16) ----------
 * 替代原 PENDING_SEND 单槽: 平台命令(SetLed) / 僵尸阈值(ZombieThreshold) /
 * 距离阈值(SensorDistance) 多来源入队, 逐条投递不覆盖 (方案 8.4.2);
 * 队列满时丢最旧保最新 (旧命令可能已被节点侧状态覆盖, 平台最新意图优先) */
#define CMD_QUEUE_SIZE  8
typedef struct {
    uint8_t nodeId;
    char    cmd[LORA_CMD_MAX_LEN];
} PendingCmd_t;
static PendingCmd_t cmdQueue[CMD_QUEUE_SIZE];
static uint8_t cmdQueueHead = 0;   /* 队首: 下一条待发送 */
static uint8_t cmdQueueTail = 0;   /* 队尾: 下一个入队槽 */
static uint8_t cmdQueueCnt  = 0;   /* 队列长度 */

/* ⭐ S33: 最近一次投递命令的目标节点与关键字.
 * 上行 ACK 帧不自带节点地址(协议缺陷), 原实现靠 currentNode 猜归属,
 * 发现扫描下命令队列投递与轮询指针推进不同步 → ACK 误归属(实测: 节点1的
 * SensorDistance 回复被记到节点2头上, 清错节点标志导致误重试2/3).
 * lastCmdNodeId 记录最近投递目标(串行协议下即"当前等待响应的节点"),
 * lastCmdKey 用于关键字双重校验(防迟到包/杂散包在指针前移后误归属).
 * 0xFF = 从未投递过命令 */
static uint8_t lastCmdNodeId = 0xFF;
static char    lastCmdKey[32] = {0};

/* ⭐ S12: OTA 期间轮询暂停/恢复.
 * OTA 开始(lora_tick 首次看到非 OTA_IDLE)时记录暂停前状态(日志用)与
 * OTA 目标节点; OTA 结束(回到 OTA_IDLE)时统一复位轮询状态机, 从节点1
 * 重新开始, 且 OTA 目标节点强制先 PING 复核 (forcePingNode), 防止升级
 * 期间残留的等待标志/过期时间戳把刚升级完的节点误判超时/离线 */
static bool     otaPausedPolling = false;
static uint8_t  savedPollNode    = 0;
static uint8_t  savedPollPhase   = 0;
static bool     savedWaitingResp = false;
static uint8_t  savedOtaNode     = 0;   /* OTA 目标节点 ID */
static uint8_t  forcePingNode    = 0;   /* 非0: 该节点下次轮询强制先 PING 再发数据 */

/* ==================== 内部函数 ==================== */

/* ⭐ S34: 日志阶段标记(枚举/声明见 lora_handler.h, 跨模块共享).
 * 原设计为阶段切换时插入空行把日志切块, 现按用户反馈停用空行输出;
 * 函数保留阶段状态记录且不再产生任何输出, 调用点保留以便需要时恢复 */
static uint8_t s_logPhase = LOGPH_NONE;

void logPhase(uint8_t ph)
{
    s_logPhase = ph;   /* 仅记录阶段, 不再输出空行 */
}

/* ⭐ S7: 阈值下发单函数. 节点确认在线(PONG,APP / 快速路径)时统一调用,
 * 消除 PONG 分支与快速路径分支两处重复实现 (方案 8.4.1):
 *   - 顺序: ZombieThreshold 在前, SensorDistance 在后
 *   - 限次重试(各最多3次), 达上限清标志并向平台补回失败结果
 * 返回 true = 已入队下发(调用方本轮不再发 DATA/CER), false = 无待下发 */
static bool pushThresholdUpdate(int slot)
{
    NodeData &nd = nodes[slot];
    bool pushed = false;

    /* ⭐ S34: 入队日志一次性概括. 两条阈值命令连续入队(本函数一轮只调一次),
     * 把原先"各打一行(待ACK)"合并成一行"入队N条: 命令1, 命令2 (重试N/3)",
     * 与实际发送解耦——实际发送由命令队列逐条投递, 投递时 sendFixedFrame
     * 再各打一行 TX, 链路往返由 TX/RX 行一一对应, 不再一条命令打三次 */
    char enqItems[80];
    int  enqItemsLen = 0;
    int  enqCount    = 0;
    int  enqRetry    = 0;
    enqItems[0] = '\0';

    /* ⭐ S32: 两条阈值命令连续入队(不再互斥短路). 原实现第一个分支
     * return true 把第二个分支短路, 导致"僵尸车阈值"发出后, "超声波阈值"
     * 要等整轮轮询再次轮到该节点才入队——两条命令间插入其他节点的
     * PING 超时重试, 日志脏且恢复配置拖到 ~4s. 命令队列逐条投递
     * (section 2 优先于轮询), 连续入队后两条命令只隔一次 ACK 往返(~100ms) */
    if (nd.thresholdNeedsUpdate && nd.certSent && nd.thresholdRetryCount < 3)
    {
        nd.thresholdRetryCount++;  /* 重试次数+1 */
        lora_sendControl(nd.nodeId, "ZombieThreshold", nd.thresholdValue);
        if (nd.thresholdRetryCount >= 3)
        {
            /* 超过3次, 清除标志, 等待用户重新下发或节点升级后重试 */
            nd.thresholdNeedsUpdate = false;
            onenet_notifyServiceResult(slot, false, 0);   /* ⭐ 回平台失败 */
            DBG_PRINTF("[LoRa] 节点%d 僵尸车阈值重试%d次失败, 放弃 (等节点升级或重新下发)\n",
                       nd.nodeId, nd.thresholdRetryCount);
        }
        else
        {
            enqItemsLen += snprintf(enqItems + enqItemsLen,
                                    sizeof(enqItems) - (size_t)enqItemsLen,
                                    "%s僵尸车阈值=%d秒",
                                    enqCount ? ", " : "", nd.thresholdValue);
            enqCount++;
            enqRetry = nd.thresholdRetryCount;
        }
        pushed = true;
    }
    if (nd.sensorDistanceNeedsUpdate && nd.certSent && nd.sensorDistanceRetryCount < 3)
    {
        nd.sensorDistanceRetryCount++;  /* 重试次数+1 */
        lora_sendControl(nd.nodeId, "SensorDistance", nd.sensorDistanceValue);
        if (nd.sensorDistanceRetryCount >= 3)
        {
            /* 超过3次, 清除标志, 等待用户重新下发 */
            nd.sensorDistanceNeedsUpdate = false;
            onenet_notifyServiceResult(slot, false, 0);   /* ⭐ 回平台失败 */
            DBG_PRINTF("[LoRa] 节点%d 超声波距离阈值重试%d次失败, 放弃 (等平台重新下发)\n",
                       nd.nodeId, nd.sensorDistanceRetryCount);
        }
        else
        {
            enqItemsLen += snprintf(enqItems + enqItemsLen,
                                    sizeof(enqItems) - (size_t)enqItemsLen,
                                    "%s超声波距离阈值=%dcm",
                                    enqCount ? ", " : "", nd.sensorDistanceValue);
            enqCount++;
            enqRetry = nd.sensorDistanceRetryCount;
        }
        pushed = true;
    }
    if (enqCount > 0)
    {
        logPhase(LOGPH_CMD);   /* ⭐ S34: 命令下发阶段 */
        DBG_PRINTF("[LoRa] 节点%d 在线, 入队%d条: %s (重试%d/3)\n",
                   nd.nodeId, enqCount, enqItems, enqRetry);
    }
    return pushed;
}

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
           (millis() - t0) <= LORA_AUX_WAIT_MS)
    {
        onenet_loop();   /* ⭐ S27: AUX忙等期间让出MQTT, 不阻塞保活 */
    }
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
           (millis() - t1) <= LORA_AUX_WAIT_MS)
    {
        onenet_loop();   /* ⭐ S27: AUX忙等期间让出MQTT */
    }   /* 等高 */
    uint8_t sawHigh = (uint8_t)digitalRead(LORA_AUX_PIN);   /* 期望 1=已变高 */
    uint32_t durHigh = millis() - t1;

    uint32_t t2 = millis();
    while (digitalRead(LORA_AUX_PIN) == HIGH &&
           (millis() - t2) <= LORA_AUX_WAIT_MS)
    {
        onenet_loop();   /* ⭐ S27: AUX忙等期间让出MQTT */
    }   /* 等低 */
    uint8_t auxAfter = (uint8_t)digitalRead(LORA_AUX_PIN);   /* 期望 0=完成 */
    uint32_t durLow = millis() - t2;

    /* === 日志输出 ===
     * 正常: 只打一行 OK(轨迹码 0->1->0 无信息量, 仅计统计数)
     * 异常: 一行 FAIL(WARN 级), 带轨迹码 + 原因 + 各阶段耗时便于定位
     * ⭐ S34: TX 行统一为 "TX -> 节点<N>: <AT命令文本> (耗时ms)", 与节点
     * ACK 回显的 RX 行一一对应; 命令文本截掉尾部 \r\n 不换行 */
    char txText[LORA_CMD_MAX_LEN + 1];
    uint16_t tlen = len;
    if (tlen > LORA_CMD_MAX_LEN) tlen = LORA_CMD_MAX_LEN;
    memcpy(txText, data, tlen);
    txText[tlen] = '\0';
    while (tlen > 0 && (txText[tlen - 1] == '\r' || txText[tlen - 1] == '\n'))
        txText[--tlen] = '\0';

    /* ⭐ S34: 依命令文本归类阶段, 供空行分隔 (PING→保活, DATA/CER→数据, 其余→命令) */
    uint8_t txPhase = LOGPH_CMD;
    if (strncmp(txText, "AT+PING", 7) == 0)
        txPhase = LOGPH_LINK;
    else if (strncmp(txText, "AT+OTA", 6) == 0)
        txPhase = LOGPH_OTA;
    else if (strncmp(txText, "AT+DATA", 7) == 0 || strncmp(txText, "AT+CER", 6) == 0)
        txPhase = LOGPH_DATA;

    bool ok = (auxBefore == LOW && sawHigh == HIGH && auxAfter == LOW);
    if (ok)
    {
        statTxOk++;
        logPhase(txPhase);
        DBG_PRINTF("[LoRa] 发送 -> 节点%u: %s (%lums)\n",
                   (unsigned int)dstAddr, txText,
                   (unsigned long)(durBefore + durHigh + durLow));
    }
    else
    {
        statTxFail++;
        const char *reason;
        if (auxBefore == HIGH)
            reason = "发前AUX忙, 模块卡在发送/接收/切换";
        else if (sawHigh == LOW)
            reason = "模块未收到数据, 串口/接线异常";
        else if (auxAfter == HIGH)
            reason = "发后AUX一直高, 模块卡死在发送中";
        else
            reason = "未知异常";
        logPhase(txPhase);
        LOG_W("[LoRa] 发送 -> 节点%u: %s FAIL [%d->%d->%d] %s (前%lu/等高%lu/等低%lu ms)\n",
              (unsigned int)dstAddr, txText, auxBefore, sawHigh, auxAfter, reason,
              (unsigned long)durBefore,
              (unsigned long)durHigh,
              (unsigned long)durLow);
        oled_showTempMessage("LoRa is OutTime!", 3000);
    }
}

/* ⭐ S33: 从投递命令提取关键字(跳过 "AT+" 前缀, 截到 '='/'\r'/'\n').
 * "AT+ZombieThreshold=5\r\n" → "ZombieThreshold", 与 ACK 回显/strstr 匹配串一致 */
static void extractCmdKey(const char *cmd, char *key, size_t keySize)
{
    const char *p = cmd;
    if (strncmp(p, "AT+", 3) == 0) p += 3;
    size_t i = 0;
    while (i < keySize - 1 && *p && *p != '=' && *p != '\r' && *p != '\n')
        key[i++] = *p++;
    key[i] = '\0';
}

/* ⭐ S33: 记录最近一次投递命令的目标节点+关键字 (ACK 归属与双重校验依据) */
static void noteCmdSent(uint8_t nodeId, const char *key)
{
    lastCmdNodeId = nodeId;
    snprintf(lastCmdKey, sizeof(lastCmdKey), "%s", key);
}

/* ⭐ S33: ACK 内容关键字与最近投递命令一致性校验.
 * ACK 帧无 CRC 且不回显参数, 只能靠"内容关键字 + 最近投递目标"双重确认.
 * 不一致 = 迟到/杂散包(指针已前移, 归属已无意义) → 拒绝处理, 防误清
 * 其他节点标志/误回平台. OTA ACK 走独立路径(ota_handler 直发不更新
 * lastCmdKey)且 nodeId 已由 OTA 目标节点覆盖 → 豁免; 从未投递过命令
 * (启动早期杂散帧)与未知内容保底放行, 不引入新误杀 */
static bool ackKeyMatches(const char *ack)
{
    if (strstr(ack, "AT+OTA") != NULL)
        return true;                     /* OTA ACK 豁免 */
    if (lastCmdKey[0] == '\0')
        return true;                     /* 从未投递命令, 保底放行 */
    if (strstr(ack, "PONG") != NULL)
        return strcmp(lastCmdKey, "PING") == 0;
    if (strstr(ack, "ZombieThreshold") != NULL)
        return strcmp(lastCmdKey, "ZombieThreshold") == 0;
    if (strstr(ack, "SetLed") != NULL)
        return strcmp(lastCmdKey, "SetLed") == 0;
    if (strstr(ack, "SensorDistance") != NULL)
        return strcmp(lastCmdKey, "SensorDistance") == 0;
    return true;                         /* 未知内容保底放行 */
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
    noteCmdSent(nodeId, prefix);   /* ⭐ S33: 记录投递目标与关键字 */
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
    /* ⭐ OTA 期间上行帧归属 OTA 目标节点: 触发命令经 sendRawFrame 直发
     * (不更新 currentNode), 且轮询已暂停, 若不覆盖归属, 触发 ACK 会被误
     * 归属为暂停前的轮询节点 → P1-5 校验拒绝 → 触发阶段必然失败
     * (实测: 节点1 的 AT+OTA:ack 被归属为节点2, 4 轮整链重试全失败,
     *  节点在 Boot 空等固件、NAK 催 5 次后放弃 = "升级失败+联系不上") */
    if (ota_getState() != OTA_IDLE)
        nodeId = ota_getProgress()->nodeId;
    /* ⭐ S33: ACK 帧不自带节点地址, 归属改用最近投递命令的目标节点.
     * 原 currentNode 归属在发现扫描下会错: 命令队列投递(节点1)后轮询指针
     * 已推进(节点2), 节点1 的 ACK 被误归属为节点2 → 清错标志/误重试
     * (实测日志: 50.342 "收到<- 节点2 确认: AT+SensorDistance" 实为节点1 回复).
     * 串行协议下 lastCmdNodeId 即"当前等待响应的节点", 归属精确 */
    else if (header == LORA_FRAME_ACK && lastCmdNodeId != 0xFF)
        nodeId = lastCmdNodeId;

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
            DBG_PRINTF("[LoRa] 收到 <- 节点%d 数据: 车位=%d 距离=%d 地磁=%d 时长=%lu LED=%d (seq=%d) RSSI=%ddBm\n",
                       nodeId, d->ParkStatus, d->Ultrasonic,
                       d->GeoMagnetic, (unsigned long)d->OccupiedTime,
                       d->LED, d->seq, rssi);
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
            DBG_PRINTF("[LoRa] 收到 <- 节点%d 证书 (有效=%d 产品=%s 设备=%s seq=%d)\n",
                       nodeId, cert->valid, cert->ProductKey, cert->DeviceName, cert->seq);
        }
        gotData = true;
        break;

    case LORA_FRAME_ACK:
        rxBuf[rxGot] = '\0';
        /* ⭐ S33: 关键字双重校验. 节点1 的回复在轮询指针前移后迟到(如命令
         * ACK 丢失→网关已超时前移→旧 ACK 才到), 若不拦会清错节点标志;
         * 与最近投递命令关键字不一致 → 视为迟到/杂散包丢弃 */
        if (!ackKeyMatches((char *)rxBuf))
        {
            statAckDrop++;
            LOG_W("[LoRa] ACK 关键字与最近投递命令不符(迟到/杂散), 丢弃: last=%s\n",
                  lastCmdKey);
            break;
        }
        gotData = true;
        /* ⭐ S34: ACK 回显与业务结果合并成一行"收到", 与"发送"行一一对应.
         * 半双工无线首字节可能被污染(实测 "�AT+ZombieThreshold"), 原样
         * 保留不过滤——乱码即链路质量证据; 关键字 strstr 匹配不受影响 */
        {
            const char *ackNote = NULL;
            uint8_t rxPhase = LOGPH_CMD;   /* ⭐ S34: 回显阶段 (PONG→保活, OTA→OTA) */
            if (strncmp((char *)rxBuf, "PONG", 4) == 0)
                rxPhase = LOGPH_LINK;
            else if (strncmp((char *)rxBuf, "AT+OTA", 6) == 0)
                rxPhase = LOGPH_OTA;
            if (strstr((char *)rxBuf, "ZombieThreshold") != NULL)
                ackNote = " (僵尸车阈值下发成功)";
            else if (strstr((char *)rxBuf, "SensorDistance") != NULL)
                ackNote = " (超声波距离阈值下发成功)";
            else if (strstr((char *)rxBuf, "SetLed") != NULL)
                ackNote = " (SetLed下发成功)";
            else if (strncmp((char *)rxBuf, "AT+OTA:version_ok", 17) == 0)
                ackNote = " (OTA版本已最新, 拒绝升级)";
            else if (strncmp((char *)rxBuf, "AT+OTA:ack", 10) == 0)
                ackNote = " (OTA触发确认)";
            logPhase(rxPhase);
            DBG_PRINTF("[LoRa] 收到 <- 节点%d: %s%s\n", nodeId, (char *)rxBuf,
                       ackNote ? ackNote : "");
        }
        /* ⭐ S31: ACK 改关键字匹配(strstr)而非全量匹配(strcmp):
         * 半双工无线链路首字节易受干扰污染(实测收到 "�AT+ZombieThreshold"),
         * 全量匹配会误判 ACK 失败 → 无谓重试下发. 命令名长且互不为子串,
         * 只要关键字未丢失即可确认成功; 匹配命令名(不含 "AT+" 前缀)
         * 进一步容忍前缀字节污染, 无跨命令误配风险 */
        /* ⭐ 阈值下发成功: 收到 ACK 后才清除标志, 重置重试计数,
         * 并向平台补回"同步服务调用"回复(成功 Result=1) */
        if (strstr((char *)rxBuf, "ZombieThreshold") != NULL)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
            {
                nodes[slot].thresholdNeedsUpdate = false;
                nodes[slot].thresholdRetryCount = 0;
                onenet_notifyServiceResult(slot, true, nodes[slot].thresholdValue);
            }
        }
        /* ⭐ LED 控制下发成功(SetLed 服务): 收到 ACK 后向平台补回"同步服务调用"回复,
         * ActualValue 回节点实际命令目标值(ledSwitch) */
        if (strstr((char *)rxBuf, "SetLed") != NULL)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
                onenet_notifyServiceResult(slot, true, nodes[slot].ledSwitch ? 1 : 0);
        }
        /* ⭐ 超声波距离阈值下发成功(SetSensorDistance 服务): 收到 ACK 后清除标志,
         * 重置重试计数, 并向平台补回"同步服务调用"回复(成功 Result=1) */
        if (strstr((char *)rxBuf, "SensorDistance") != NULL)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
            {
                nodes[slot].sensorDistanceNeedsUpdate = false;
                nodes[slot].sensorDistanceRetryCount = 0;
                onenet_notifyServiceResult(slot, true, nodes[slot].sensorDistanceValue);
            }
        }
        /* ⭐ OTA 触发命令回复解析 (方案 8.1#2 / S21):
         * 必须先判 version_ok 再判 ack (两前缀同源, 需最具体者先行):
         *   - AT+OTA:version_ok (节点 App 版本已最新, 拒绝升级)
         *     → ota_notifyVersionOk 标记 otaRefused 并终止 OTA 流,
         *       不再对 App 发 Xmodem 包 (旧实现误当 TriggerAck 处理,
         *       网关继续空转 = 整链空转/空口噪声根因)
         *   - AT+OTA:ack (正常触发确认)
         *     → ota_notifyTriggerAck 置位 s_triggerAcked,
         *       网关从 OTA_TRIGGER_NODE 进入复位等待.
         *       之前该分支缺失 → s_triggerAcked 永远为 false → 触发阶段
         *       必然超时重发 3 次后放弃, 而节点已擦除 APP 区在裸等
         *       = "节点死亡"直接根因. */
        if (strncmp((char *)rxBuf, "AT+OTA:version_ok", 17) == 0)
            ota_notifyVersionOk(nodeId);
        else if (strncmp((char *)rxBuf, "AT+OTA:ack", 10) == 0)
            ota_notifyTriggerAck(nodeId);   /* ⭐ P1-5: 传目标节点, 供判定器校验 */
        /* ⭐ S9: PING 通(PONG)即视为节点链路存活. 解析 PONG[,<MODE>] (方案 5.1/5.2):
         *   纯 "PONG"(旧固件) / "PONG,APP" → APP; "PONG,BOOT" → BOOT.
         * 统一经 updateNodeState 更新三态 (原 wasOffline/online 散落赋值删除,
         * "离线恢复→重新代上线"逻辑已内聚到 updateNodeState) */
        if (strncmp((char *)rxBuf, "PONG", 4) == 0)
        {
            int slot = findNode(nodeId);
            if (slot >= 0)
            {
                const char *mode = (const char *)rxBuf + 4;   /* 指向 ",BOOT"/",APP"/"" */
                bool isBoot = (strncmp(mode, ",BOOT", 5) == 0);
                uint8_t prevMode = nodes[slot].mode;  /* ⭐ P1-4: 记录跃迁前模式, 仅状态变化时打印 */
                updateNodeState((uint8_t)slot,
                                isBoot ? NODE_EVT_PONG_BOOT : NODE_EVT_PONG_APP);

                /* ⭐ S15: 节点确认进入 APP (PONG,APP) → 清理最近一次 OTA 的固件文件 */
                if (!isBoot)
                    ota_notifyNodeApp(nodeId);

                /* Boot 模式: 不索数据不下发配置, 只保活 + 转 OTA 判定器 (7.1#1) */
                if (isBoot)
                {
                    /* ⭐ P1-4 降噪: 每轮 PING(~2s) 都打印"转 OTA 判定器"是纯日志噪音,
                     * 仅当节点模式由非 BOOT 跃迁到 BOOT(首次进入/重新进入)时打印一次 */
                    if (prevMode != NODE_MODE_BOOT)
                        DBG_PRINTF("[LoRa] 节点%d 进入 BOOT 模式, 转 OTA 判定器\n", nodeId);
                    /* ⭐ S14/8.1 统一判定器: 有固件→自动重发(自愈); 无固件→bootPending 等平台 */
                    ota_autoDispatch(nodeId);
                    break;   /* 跳出 ACK 分支, 跳过下方仅 App 模式执行的配置下发 */
                }

                /* ⭐ S30: PONG 兜底重发. 所有"重新上线"场景(节点断电重连/重启、
                 * 网关断电重启、OTA 成功后节点重启)都汇合于此——节点必然先被
                 * PING 再回 PONG,APP. 此处统一按内存/LFS 保存的用户设置重新置位,
                 * 一处覆盖全部重启场景 (替代原 LFS 加载与 OTA 成功后两处散落置位).
                 * 仅当用户设置过(Value>0)才重发, 未设置过不打扰 */
                if (nodes[slot].thresholdValue > 0)
                {
                    nodes[slot].thresholdNeedsUpdate = true;
                    nodes[slot].thresholdRetryCount = 0;
                }
                if (nodes[slot].sensorDistanceValue > 0)
                {
                    nodes[slot].sensorDistanceNeedsUpdate = true;
                    nodes[slot].sensorDistanceRetryCount = 0;
                }
                /* ⭐ S7: 阈值下发统一走单函数 (与快速路径分支共用,
                 * 消除双实现; 返回 true=已入队下发) */
                pushThresholdUpdate(slot);
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
                   :                              (uint16_t)(RX_BUF_SIZE - 1);   /* ⭐ S17: ACK 显式长度上限 */
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
    waitingFastPath = false;    /* 防御: 换地址不残留快速路径等待标志 (S9) */
    pollPhase       = 0;   /* 换地址时复位轮询阶段, 防止 PONG 已收(阶段2)
                            * 被下一个节点继承, 跳过 PING 直接发 CER/DATA */
    currentNode++;
    if (currentNode > LORA_POLL_TO_NODE)
    {
        currentNode = LORA_POLL_FROM_NODE;
        roundStartAt = millis();   /* 新一轮开始计时 (正常轮询节流) */
        /* ⭐ S13: 一轮所有节点扫完 → 按"轮询轮次"统计离线 (替代墙钟超时) */
        nodeRoundCompleted();
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
        statRxReset++;
        DBG_PRINTF("[LoRa] RX 状态机超时 (%s, %lums), 强制复位\n",
                   rxStateName(rxState), (unsigned long)(now - lastRxByteMs));
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
                waitingFastPath = false;  /* 收到响应, 快速路径等待结束 (S9) */
                if (ota_getState() != OTA_IDLE)
                {
                    /* ⭐ OTA 进行中: 轮询已暂停, 收到 OTA 暂停前在途的旧响应
                     * 只清等待标志, 不推进轮询节点. 否则 currentNode 被推进,
                     * 与 OTA 目标错位, 触发 ACK 会被误归属为他节点 */
                }
                else if (waitingCmdReply)
                {
                    /* ⭐ S35: 队列命令回执: 只清等待, 不推进轮询节点;
                     * 恢复投递前的 pollPhase(PONG后=2), 下一轮 POLL 分支
                     * 正常发真实命令或前移到下一地址, 防止命令回执消耗
                     * 扫描地址导致搜索模式跳过节点提前退出 */
                    waitingCmdReply = false;
                    pollPhase = savedPollPhaseBeforeCmd;
                }
                else if (pollPhase == 1)
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
        /* ⭐ S12: 首次暂停时保存轮询上下文与 OTA 目标节点 (恢复用) */
        if (!otaPausedPolling)
        {
            otaPausedPolling = true;
            savedPollNode    = currentNode;
            savedPollPhase   = pollPhase;
            savedWaitingResp = waitingResp;
            savedOtaNode     = ota_getProgress()->nodeId;
            DBG_PRINTF("[LoRa] OTA 开始(节点%d), 暂停轮询 (暂停前: 节点%d 阶段%d 等响应=%d)\n",
                       savedOtaNode, savedPollNode, savedPollPhase, savedWaitingResp);
        }
        return gotData;
    }

    /* ⭐ S12: OTA 刚结束 → 统一复位轮询状态机, 强制从 PING 探测重新开始.
     * 升级期间节点复位重启, 残留的 waitingResp/waitingFastPath 会在恢复瞬间
     * 触发"超时/数据丢失"误判; roundStartAt/cmdSentAt 也是过期时间戳;
     * 全部复位后从节点1重新轮询, OTA 目标节点强制先 PING 重探,
     * 新固件模式(PONG,APP/BOOT)自然重新建立 (8.1 自愈判定器依赖此探测) */
    if (otaPausedPolling)
    {
        otaPausedPolling = false;
        waitingResp      = false;
        waitingFastPath  = false;
        waitingCmdReply  = false;   /* ⭐ S35: 防御, 清命令回执标志避免跨OTA残留 */
        pollPhase        = 0;
        discoverPingAttempts = 0;
        currentNode      = LORA_POLL_FROM_NODE;
        roundStartAt     = now;          /* 新一轮节流从此刻起算 */
        if (savedOtaNode != 0)
            forcePingNode = savedOtaNode;   /* OTA 目标节点下次轮询先 PING 复核 */
        DBG_PRINTF("[LoRa] OTA 结束, 轮询状态复位, 从节点%d 重新轮询%s\n",
                   LORA_POLL_FROM_NODE, forcePingNode ? " (OTA目标节点强制先PING)" : "");
    }

    /* --- 2. 处理待发控制命令 (优先于轮询, 且不等待响应不算节点轮询) ---
     * S16: 取队首一条逐条投递, 队列化不覆盖 */
    if (cmdQueueCnt > 0 && !waitingResp)
    {
        PendingCmd_t *pc = &cmdQueue[cmdQueueHead];
        sendFixedFrame((uint16_t)pc->nodeId, LORA_CHANNEL,
                       (const uint8_t *)pc->cmd, strlen(pc->cmd));
        cmdQueueHead = (cmdQueueHead + 1) % CMD_QUEUE_SIZE;
        cmdQueueCnt--;
        cmdSentAt = now;
        waitingResp = true;
        respTimeout = LORA_RESPONSE_TIMEOUT_MS;  /* 控制命令用正常超时 */
        /* ⭐ S35: 先保存投递前轮询阶段(PONG后=2), 命令回执后恢复, 使搜索
         * 模式 PONG 分支能正常前移到下一地址; 回执不再推进轮询节点 */
        savedPollPhaseBeforeCmd = pollPhase;
        waitingCmdReply = true;
        pollPhase   = 0;                         /* 控制命令不参与 PING 阶段 */
        waitingFastPath = false;                 /* 控制命令非快速路径, 不计 dataMiss */
        /* ⭐ S33: 队列命令投递也记录目标与关键字(ACK 归属/校验依据) */
        {
            char keyBuf[32];
            extractCmdKey(pc->cmd, keyBuf, sizeof(keyBuf));
            noteCmdSent(pc->nodeId, keyBuf);
        }
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
                noteCmdSent(currentNode, "PING");   /* ⭐ S33 */
                /* sendFixedFrame 已打印"发送"行, 不再补打 PING-> 尝试日志,
                 * 避免同毫秒两行重复 (计数 x/3 分母含首次还永远到不了 3/3) */
                return gotData;
            }
            /* ⭐ S9 快速路径(DATA/CER)超时: 节点"链路还热但业务不答" — 典型
             * 僵尸/半死通道. dataMissCount+1, 达 DATA_MISS_OFFLINE_MAX 次后
             * serviceOnline 被摘除, 立即降级为 PING 探测复核 (方案 7.1#3) */
            if (waitingFastPath)
            {
                waitingFastPath = false;
                int slot = findNode(currentNode);
                if (slot >= 0)
                {
                    updateNodeState((uint8_t)slot, NODE_EVT_DATA_MISS);
                    if (nodes[slot].dataMissCount >= DATA_MISS_OFFLINE_MAX)
                    {
                        offlineProbeAt[currentNode] = millis(); /* 下一轮立即 PING 复核 */
                        logPhase(LOGPH_LINK);   /* ⭐ S34: 链路保活阶段 */
                        DBG_PRINTF("[LoRa] 节点%d 快速路径连续%d次无响应, 降级为 PING 探测\n",
                                   currentNode, nodes[slot].dataMissCount);
                    }
                }
            }
            statRespTimeout++;
            LOG_W("[LoRa] 节点%d 超时 (跳过)\n", currentNode);
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
            waitingCmdReply = false;   /* ⭐ S35: 命令回执超时按普通超时处理 */
            pollPhase   = 0;     /* 超时 → 重置阶段, 正常前移 */
            discoverPingAttempts = 0;
            advanceNextNode();
        }
        return gotData;   /* 等当前响应, 暂不发下一条 */
    }

    /* --- 4. 发下一条轮询命令 --- */
    {
        /* ⭐ S31: 先代下线再探测 — MQTT 会话(重)建后 logoutPending 已对所有
         * 已注册节点置位(平台会话里节点仍显示在线). 若此时先发 PING 会形成
         * "先探测后下线"的顺序颠倒与日志交错(实测: PING 发出后 ~60ms 才发下线,
         * 平台短暂保持伪在线). 在全部代下线完成前暂停发送下一条轮询命令
         * (onenet_uploadAll 每拍清一条, 控制命令队列不受影响);
         * 仅 MQTT 在线时生效, 断网时照常轮询保持本地状态 */
        if (onenet_logoutPending())
        {
            return gotData;
        }

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

        if (pollPhase == 2)
        {
            /* ⭐ 7.2#2: Boot 节点只 PING — PONG,BOOT 已确认保活, 不索数据
             * 不下发配置, 直接前移保持正常节奏; OTA 判定交给用户级决策器 */
            if (slot >= 0 && nodes[slot].mode == NODE_MODE_BOOT)
            {
                advanceNextNode();
            }
            /* PONG 已收, 发真实命令:
             *   搜索模式: 无证书 → CER 注册; 有证书 → PING 已确认在线, 直接跳过
             *   正常模式: 未注册 → CER; 已注册 → DATA (或每50次夹发CER校验) */
            else if (discoveryMode)
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
            noteCmdSent(currentNode, "PING");   /* ⭐ S33 */
            if (currentNode == forcePingNode) forcePingNode = 0;   /* ⭐ S12: 已PING复核 */
        }
        else if (certOk && slot >= 0 &&
                 nodes[slot].mode == NODE_MODE_APP && nodes[slot].serviceOnline &&
                 currentNode != forcePingNode)   /* ⭐ S12: OTA 目标节点强制先 PING 复核 */
        {
            /* 已注册 APP 在线节点: 快速路径, 直接发 DATA (不经过 PING);
             * 仅当三态=链路活+模式APP+业务在线才走此通道 (S9) */
            /* ⭐ S7: 阈值下发统一走单函数 (与 PONG 分支共用);
             * 返回 true=已入队下发, 本轮不再发 DATA/CER
             * (命令队列在后续 lora_tick 中投递, 避免抢发) */
            if (!pushThresholdUpdate(slot))
            {
                bool verify = shouldVerifyCert(currentNode);
                sendAT(currentNode, verify ? "CER" : "DATA", 0, false);
                respTimeout = LORA_RESPONSE_TIMEOUT_MS;
                waitingFastPath = true;   /* 快速路径 DATA/CER 待响应, 超时计入 dataMiss (S9) */
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
            /* 正常模式: BOOT / UNKNOWN / APP(业务离线) 节点先 PING 确认链路;
             * 离线退避: 非 BOOT 未到下次探测时刻(offlineProbeAt)直接跳过,
             * 离线越久探测间隔越稀疏, 避免长期离线节点拖慢轮询一圈;
             * BOOT 节点不退避, 保持正常轮询节奏 (方案 7.2#2) */
            if (slot >= 0 && nodes[slot].mode != NODE_MODE_BOOT &&
                now < offlineProbeAt[currentNode])
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
            noteCmdSent(currentNode, "PING");   /* ⭐ S33 */
            if (currentNode == forcePingNode)
            {
                logPhase(LOGPH_LINK);   /* ⭐ S34: OTA 后 PING 复核, 链路保活阶段 */
                DBG_PRINTF("[LoRa] 节点%d OTA 后强制 PING 复核\n", currentNode);
                forcePingNode = 0;
            }
        }
    }

    /* --- 5. 链路统计周期报告: 每 STAT_REPORT_MS 汇总一行, 长跑时一眼看出
     * 空口质量(丢包率/超时/状态机复位/ACK丢弃), 无需翻整段日志 --- */
    if (statLastReport == 0 || (now - statLastReport) >= STAT_REPORT_MS)
    {
        if (statLastReport != 0)   /* 启动首个周期不报, 等有真实数据 */
        {
            uint32_t total = statTxOk + statTxFail;
            if (total > 0)
            {
                uint32_t loss = (statTxFail * 100) / total;
                logPhase(LOGPH_STAT);   /* ⭐ S34: 周期统计独立成块 */
                DBG_PRINTF("[LoRa] 链路统计 %us: 发送=%u(成功%u/失败%u,丢%u%%) "
                           "响应超时=%u 状态机复位=%u ACK丢弃=%u\n",
                           STAT_REPORT_MS / 1000U,
                           (unsigned)total, (unsigned)statTxOk, (unsigned)statTxFail,
                           (unsigned)loss,
                           (unsigned)statRespTimeout, (unsigned)statRxReset,
                           (unsigned)statAckDrop);
            }
        }
        statLastReport = now;
    }

    return gotData;
}

/* 手动触发节点发现: 从头扫描所有节点 (进入搜索模式, 扫完一轮自动退出) */
void lora_triggerDiscovery(void)
{
    currentNode = LORA_POLL_FROM_NODE;
    waitingResp = false;
    waitingFastPath = false;    /* ⭐ S35: 防御, 重新扫描前清残留等待标志 */
    waitingCmdReply = false;    /* ⭐ S35: 防命令回执残留吞掉本轮 PONG */
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
     * 节点端回调按纯名称匹配 (如 "AT+SetLed"), 节点地址靠定点传输[AddrH][AddrL]区分,
     * 命令名不包含 nodeId. S16: 环形队列入队, 多来源命令不互相覆盖 */
    if (cmdQueueCnt >= CMD_QUEUE_SIZE)
    {
        /* 队列满: 丢最旧保最新, 打日志便于排查积压 */
        DBG_PRINTF("[LoRa] 命令队列满(%d), 丢弃最旧: %s", CMD_QUEUE_SIZE,
                   cmdQueue[cmdQueueHead].cmd);
        cmdQueueHead = (cmdQueueHead + 1) % CMD_QUEUE_SIZE;
        cmdQueueCnt--;
    }
    PendingCmd_t *pc = &cmdQueue[cmdQueueTail];
    int n = snprintf(pc->cmd, sizeof(pc->cmd), "AT+%s=%d\r\n", property, value);
    (void)n;
    pc->nodeId = nodeId;
    cmdQueueTail = (cmdQueueTail + 1) % CMD_QUEUE_SIZE;
    cmdQueueCnt++;
}

Stream &lora_getSerial(void) { return loraSerial; }
