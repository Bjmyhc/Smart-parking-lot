/* node_data.cpp - Gateway 节点数据缓冲区实现 */
#include "node_data.h"
#include "app_cfg.h"      /* LORA_MAX_NODES/sysEventFlag/DBG */
#include <LittleFS.h>

/* ==================== 证书持久化 (LittleFS) ====================
 * 每次收到节点证书后保存到 Flash, 重启后直接加载, 免等 LoRa 重新上报 */
#define CERTS_FILE  "/certs.dat"

typedef struct {
    uint8_t  nodeId;               /* 节点ID 1..N */
    char     productKey[12];       /* 子设备产品ID */
    char     deviceName[33];       /* 子设备设备名 */
    bool     certSent;             /* 是否收到过证书 */
    uint32_t zombieThresholdSec;   /* 僵尸车判定阈值(秒), 0=未设置/使用默认 */
    uint16_t sensorDistanceCm;     /* ⭐ 超声波距离阈值(cm), 0=未设置/使用默认 */
} PersistedCert_t;

static const uint8_t CERTS_MAGIC = 0xA6;  /* 文件有效性标识 (含sensorDistanceCm) */

/* ⭐ S19: 证书/阈值持久化脏标记. 变更只置位, 由主循环 persistCertsIfDirty()
 * 统一异步落盘, 消除 Flash 擦写在 LoRa 轮询/MQTT 回调链中的同步阻塞 */
static bool s_certsDirty = false;

void certsMarkDirty(void)   { s_certsDirty = true; }

void persistCertsIfDirty(void)
{
    if (!s_certsDirty) return;   /* 无变更: 零开销 */
    s_certsDirty = false;
    saveCertsToLittleFS();       /* 有变更才写 Flash (节流: 仅变更后写) */
}

void saveCertsToLittleFS(void)
{
    if (!LittleFS.begin()) return;
    File f = LittleFS.open(CERTS_FILE, "w");
    if (!f) { LittleFS.end(); return; }

    f.write(CERTS_MAGIC);
    /* 先统计实际保存条数 (只存 certSent=true 的节点) */
    uint8_t saved = 0;
    for (uint8_t i = 0; i < nodeCount; i++)
        if (nodes[i].certSent) saved++;
    f.write(saved);
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (!nodes[i].certSent) continue;
        PersistedCert_t pc;
        pc.nodeId   = nodes[i].nodeId;
        pc.certSent = true;
        memcpy(pc.productKey, nodes[i].productKey, sizeof(pc.productKey));
        memcpy(pc.deviceName, nodes[i].deviceName, sizeof(pc.deviceName));
        /* ⭐ 保存僵尸车阈值: 只有已设置的节点才保存 (非0值) */
        pc.zombieThresholdSec = (nodes[i].thresholdValue > 0) ?
                                nodes[i].thresholdValue : 0;
        /* ⭐ 保存超声波距离阈值 */
        pc.sensorDistanceCm = (nodes[i].sensorDistanceValue > 0) ?
                              nodes[i].sensorDistanceValue : 0;
        f.write((uint8_t *)&pc, sizeof(PersistedCert_t));
    }
    f.close();
    LittleFS.end();
    DBG_PRINTF("[LFS] 已保存 %d 条证书(含阈值)\n", saved);
}

void loadCertsFromLittleFS(void)
{
    if (!LittleFS.begin()) return;
    if (!LittleFS.exists(CERTS_FILE)) { LittleFS.end(); return; }

    File f = LittleFS.open(CERTS_FILE, "r");
    if (!f) { LittleFS.end(); return; }

    uint8_t magic = f.read();
    if (magic != CERTS_MAGIC) { f.close(); LittleFS.end(); return; }

    uint8_t count = f.read();
    for (uint8_t i = 0; i < count; i++)
    {
        PersistedCert_t pc;
        if (f.read((uint8_t *)&pc, sizeof(PersistedCert_t)) != sizeof(PersistedCert_t))
            break;
        if (!pc.certSent) continue;

        /* 注册节点并写证书 */
        int slot = findNode(pc.nodeId);
        if (slot < 0) slot = registerNode(pc.nodeId);
        if (slot < 0) continue;

        nodes[slot].certSent = true;
        /* 存证不代在线: linkAlive=false/mode=UNKNOWN (registerNode 已清零),
         * 等收到 LoRa 应答 (PONG/DATA/CERT) 才由 updateNodeState 激活 */
        memcpy(nodes[slot].productKey, pc.productKey, sizeof(nodes[slot].productKey));
        memcpy(nodes[slot].deviceName, pc.deviceName, sizeof(nodes[slot].deviceName));
        nodes[slot].loginPending = true;  /* 重启后需重新代上线 */
        sysEventFlag |= (1 << (pc.nodeId - 1));
        DBG_PRINTF("[LFS] 加载证书: 节点%d (%s/%s)\n",
                   pc.nodeId, pc.productKey, pc.deviceName);

        /* ⭐ S30: 只加载阈值目标值, 不再置位下发标志. 重发统一由"节点 PONG
         * 重新上线时兜底"(lora_handler PONG,APP 分支)触发——网关断电重启、
         * 节点断电重连/重启、OTA 成功后节点重启都会先 PING 再 PONG, 一处覆盖 */
        if (pc.zombieThresholdSec > 0)
        {
            nodes[slot].thresholdValue = pc.zombieThresholdSec;
            DBG_PRINTF("[LFS] 节点%d 僵尸车阈值=%lu秒 (待PONG兜底重发)\n",
                       pc.nodeId, (unsigned long)pc.zombieThresholdSec);
        }
        /* ⭐ 超声波距离阈值同理: 只加载目标值, 待 PONG 兜底重发 */
        if (pc.sensorDistanceCm > 0)
        {
            nodes[slot].sensorDistanceValue = pc.sensorDistanceCm;
            DBG_PRINTF("[LFS] 节点%d 超声波距离阈值=%ucm (待PONG兜底重发)\n",
                       pc.nodeId, pc.sensorDistanceCm);
        }
    }
    f.close();
    LittleFS.end();
    dataChanged = true;
    DBG_PRINTF("[LFS] 已从Flash加载 %d 条证书(含阈值)\n", count);
}

NodeData nodes[LORA_MAX_NODES];
uint8_t  nodeCount   = 0;
bool     dataChanged = false;

int findNode(uint8_t nodeId)
{
    for (uint8_t i = 0; i < nodeCount; i++)
        if (nodes[i].nodeId == nodeId) return i;
    return -1;
}

int registerNode(uint8_t nodeId)
{
    if (nodeId < 1 || nodeId > LORA_MAX_NODES) return -1;
    if (nodeCount >= LORA_MAX_NODES) return -1;

    int slot = nodeCount;
    memset(&nodes[slot], 0, sizeof(NodeData));
    nodes[slot].nodeId     = nodeId;
    nodes[slot].lastUpdate = millis();
    /* 三态状态保持 memset 初值: linkAlive=false/mode=UNKNOWN/serviceOnline=false,
     * 由调用方随后 updateNodeState(收到帧) 或等待首次 LoRa 应答激活 */
    nodes[slot].certSent   = false;
    nodeCount++;
    DBG_PRINTF("[节点] 注册 节点%d (槽位 %d, 共 %d)\n",
               nodeId, slot, nodeCount);
    return slot;
}

void updateNodeFromRaw(uint8_t nodeId, const LoraNodeData_t *raw)
{
    int slot = findNode(nodeId);
    if (slot < 0) slot = registerNode(nodeId);
    if (slot < 0) return;

    NodeData &nd = nodes[slot];

    /* ⭐ v2 双保险: 即便 lora_handler.cpp handleCompleteFrame 已校验过 CRC + 字段,
     * 这里再做一次截断兜底, 防止任何路径漏判导致垃圾值上报到 OneNET 平台.
     * 历史乱码 bug 根因之一: 数据帧错位解析后 ParkStatus=47/Ultrasonic=6530 等
     * 垃圾值被原样存入 NodeData, 平台物模型收到非法值引发连锁异常 */
    uint8_t  parkStatus   = raw->ParkStatus;
    uint16_t ultrasonic   = raw->Ultrasonic;
    uint32_t occupiedTime = raw->OccupiedTime;
    if (parkStatus > 2)    { parkStatus    = 2; }   /* 截断到最大合法值 */
    if (ultrasonic > 1000) { ultrasonic    = 1000; }   /* 距离上限 1000cm */
    if (occupiedTime > 86400) { occupiedTime = 86400; } /* 时长上限 1 天 */

    /* ⭐ S23: 全字段 dirty 对比 — 任何业务字段变化都触发立即上报,
     * 不再只看 parkStatus (OccupiedTime 等字段截断变化此前不触发上报, 数据陈旧);
     * 上行已有 UPLOAD_MIN_INTERVAL_MS(1s) 闸门限频 + UPLOAD_INTERVAL(5s) 定时兜底,
     * 距离微小抖动不会刷屏平台. rssi 每帧都变且非业务字段, 不参与对比 */
    bool changed =
        (nd.parkStatus         != parkStatus) ||
        (nd.ultrasonic         != ultrasonic) ||
        (nd.occupiedTime       != occupiedTime) ||
        (nd.geoMagnetic        != (raw->GeoMagnetic != 0)) ||
        (nd.led                != (raw->LED != 0)) ||
        (nd.zombieThresholdSec != raw->ZombieThreshold) ||
        (nd.sensorDistanceCm   != raw->SensorDistanceCm);

    nd.parkStatus   = parkStatus;
    nd.geoMagnetic   = (raw->GeoMagnetic != 0);   /* 任意非零值转 0/1 */
    nd.ultrasonic   = ultrasonic;
    nd.occupiedTime = occupiedTime;
    nd.led          = (raw->LED != 0);
    nd.zombieThresholdSec = raw->ZombieThreshold;   /* ⭐ 节点当前生效阈值, 供 pack/post 上报观看 */
    nd.sensorDistanceCm   = raw->SensorDistanceCm;   /* ⭐ 节点当前生效超声波距离阈值 */
    /* ⭐ S29: 收到首帧业务数据 → 允许代子设备上报属性.
     * (上报门控 hasDataFrame: 节点上线但数据帧未到时拦截, 避免全 0 垃圾快照) */
    nd.hasDataFrame = true;
    updateNodeState((uint8_t)slot, NODE_EVT_DATA);   /* 统一更新 linkAlive/mode/serviceOnline/lastUpdate (S9) */
    sysEventFlag |= (1 << (nd.nodeId - 1));   /* 同步事件标志位 */

    if (changed) dataChanged = true;
}

void updateNodeCert(uint8_t nodeId, const LoraNodeCert_t *cert)
{
    int slot = findNode(nodeId);
    if (slot < 0) slot = registerNode(nodeId);
    if (slot < 0) return;

    NodeData &nd = nodes[slot];

    /* 记录旧证书, 用于对比 (周期校验发现同地址换节点) */
    char oldPk[sizeof(nd.productKey)];
    char oldDn[sizeof(nd.deviceName)];
    memcpy(oldPk, nd.productKey, sizeof(oldPk));
    memcpy(oldDn, nd.deviceName, sizeof(oldDn));
    bool certChanged = nd.certSent &&
                       ((memcmp(oldPk, cert->ProductKey, sizeof(oldPk)) != 0) ||
                        (memcmp(oldDn, cert->DeviceName, sizeof(oldDn)) != 0));
    bool wasCertSent = nd.certSent;   /* ⭐ S19: 首次收到判断用旧值 */

    nd.certSent   = true;
    /* 收到证书 = 链路活性确认 (S9): 证书只有 App 上报 → 链路活 + 模式=APP;
     * subLogin/loginPending 由下方证书内容校验逻辑管理 */
    updateNodeState((uint8_t)slot, NODE_EVT_CERT);
    nd.subLogin   = false;        /* 证书更新后需要重新代上线 */
    strncpy(nd.productKey, cert->ProductKey, sizeof(nd.productKey) - 1);
    nd.productKey[sizeof(nd.productKey) - 1] = '\0';
    strncpy(nd.deviceName, cert->DeviceName, sizeof(nd.deviceName) - 1);
    nd.deviceName[sizeof(nd.deviceName) - 1] = '\0';
    strncpy(nd.fwVersion, cert->FwVersion, sizeof(nd.fwVersion) - 1);
    nd.fwVersion[sizeof(nd.fwVersion) - 1] = '\0';   /* ⭐ 节点固件版本从证书帧取, OTA 检测据此自动更新 */

    /* 证书有效且信息完整才允许代上线 */
    nd.loginPending = (cert->valid != 0) &&
                      (nd.productKey[0] != '\0') &&
                      (nd.deviceName[0] != '\0');
    nd.logoutPending = false;
    dataChanged = true;

    /* ⭐ S19: 证书已更新 -> 置脏标记, 主循环 persistCertsIfDirty 异步落盘.
     * 不再同步写 Flash (擦写耗时阻塞 LoRa 轮询/MQTT 回调时序);
     * 仅首次收到或内容变化才置脏 (周期 CER 校验重复上报相同证书不落盘 = 节流,
     * 减少 Flash 磨损; 阈值变更由 onenet_handler 侧 certsMarkDirty 覆盖) */
    if (!wasCertSent || certChanged)
        certsMarkDirty();

    /* 同地址换新节点: 打印变更日志 */
    if (certChanged)
        DBG_PRINTF("[节点] 节点%d 证书变更: %s/%s -> %s/%s\n",
                   nodeId, oldPk, oldDn,
                   cert->ProductKey, cert->DeviceName);
}

/* ⭐ 三态状态统一赋值入口 (S9): linkAlive/mode/serviceOnline 只经此修改.
 * 事件驱动: 收到帧(PONG/DATA/CERT)/快速路径超时(DATA_MISS)/超时窗口(TIMEOUT).
 * 置 true 与置 false 的路径在此成对出现, 消除散落赋值点.
 * 返回 true = 节点由"链路死"转"链路活"(离线恢复跃迁) */
bool updateNodeState(uint8_t slot, uint8_t event)
{
    if (slot >= nodeCount) return false;
    NodeData &nd = nodes[slot];
    bool wasAlive = nd.linkAlive;

    switch (event)
    {
    case NODE_EVT_PONG_APP:   /* PONG / PONG,APP: 链路活 + 模式=APP + 业务在线 */
        nd.linkAlive     = true;
        nd.mode          = NODE_MODE_APP;
        nd.serviceOnline = true;
        nd.dataMissCount = 0;
        nd.lastUpdate    = millis();
        break;

    case NODE_EVT_PONG_BOOT:  /* PONG,BOOT: 链路活 + 模式=BOOT + 业务摘除(不索数据) */
        nd.linkAlive     = true;
        nd.mode          = NODE_MODE_BOOT;
        nd.serviceOnline = false;
        nd.dataMissCount = 0;
        nd.lastUpdate    = millis();
        break;

    case NODE_EVT_DATA:       /* 业务数据帧: 只有 App 上报 → 链路活 + 模式=APP + 业务在线
                               * (之前误判 BOOT? 收到数据帧即说明已升级/运行到 App) */
        nd.linkAlive     = true;
        nd.mode          = NODE_MODE_APP;
        nd.serviceOnline = true;
        nd.dataMissCount = 0;
        nd.lastUpdate    = millis();
        break;

    case NODE_EVT_CERT:       /* 证书帧: 只有 App 上报 → 链路活 + 模式=APP.
                               * serviceOnline 不强制改: 证书后还需 PONG/DATA 确认业务就绪 */
        nd.linkAlive     = true;
        nd.mode          = NODE_MODE_APP;
        nd.dataMissCount = 0;
        nd.lastUpdate    = millis();
        break;

    case NODE_EVT_TIMEOUT:    /* 超时窗口到期: 链路死 + 业务摘除 (mode 保留, 恢复后沿用) */
        nd.linkAlive     = false;
        nd.serviceOnline = false;
        nd.dataMissCount = 0;
        break;

    case NODE_EVT_DATA_MISS:  /* 快速路径 DATA/CER 超时: 计数+1, 达上限业务摘除转 PING */
        if (nd.linkAlive)
        {
            nd.dataMissCount++;
            if (nd.dataMissCount >= DATA_MISS_OFFLINE_MAX)
                nd.serviceOnline = false;
        }
        break;

    default:
        break;
    }

    /* ⭐ S14: 节点确认运行 App(收到 PONG/DATA/CERT) → 已离开 Boot, 不再等待 OTA */
    if (nd.mode == NODE_MODE_APP)
        nd.bootPending = false;

    /* 离线恢复跃迁: 链路死 → 活, 已拿证书且未代上线 → 重新标记待代上线 */
    bool revived = !wasAlive && nd.linkAlive;
    if (revived && nd.certSent && !nd.subLogin)
        nd.loginPending = true;

    return revived;
}

/* ⭐ S13: 节点离线判定改用"轮询轮次"计 (替代原墙钟动态超时).
 * 每轮轮询结束(advanceNextNode 回绕)由 lora_handler 调用一次:
 *   - 本轮(lastUpdate >= 上轮边界)有响应的节点 → missedRounds 清零
 *   - 本轮无响应的节点 → missedRounds+1, 连续 NODE_MISSED_ROUNDS_MAX 轮判离线
 * 相比旧墙钟超时(timeout = 2*(base + 节点数*每节点耗时))的优势:
 *   - 不受轮询一圈耗时/节点数量影响, 多节点下不会因轮询慢而误判
 *   - OTA 暂停期 lora_tick 提前返回, 无轮次边界触发, 天然不误判升级中的节点
 *   - 单帧丢失后下一轮 PING 复核成功即清零, 容忍偶发丢帧 */
static uint32_t s_roundBoundaryMs = 0;   /* 上一轮轮询结束时刻(本轮起点) */

void nodeRoundCompleted(void)
{
    uint32_t now = millis();
    if (s_roundBoundaryMs == 0)   /* 首轮: 无上轮边界可对比, 仅记下边界 */
    {
        s_roundBoundaryMs = now;
        return;
    }

    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (!nodes[i].linkAlive) continue;   /* 已离线节点不再累计 */

        if (nodes[i].lastUpdate >= s_roundBoundaryMs)
        {
            /* 本轮收到过任何有效响应 (PONG/DATA/CERT) → 清零 */
            nodes[i].missedRounds = 0;
        }
        else
        {
            nodes[i].missedRounds++;
            if (nodes[i].missedRounds >= NODE_MISSED_ROUNDS_MAX)
            {
                /* 连续 N 轮轮询无响应: 判离线 (逻辑同旧 checkNodeTimeout) */
                updateNodeState(i, NODE_EVT_TIMEOUT);
                sysEventFlag &= ~(1 << (nodes[i].nodeId - 1));  /* 清除事件标志位 */
                /* 已代上线过, 需要通知平台子设备离线 */
                if (nodes[i].subLogin)
                {
                    nodes[i].logoutPending = true;
                    nodes[i].subLogin = false;
                }
                nodes[i].loginPending = false;
                nodes[i].missedRounds = 0;
                dataChanged = true;
                DBG_PRINTF("[节点] 节点%d 离线 (连续%u轮轮询无响应)\n",
                           nodes[i].nodeId, (unsigned)NODE_MISSED_ROUNDS_MAX);
            }
        }
    }
    s_roundBoundaryMs = now;   /* 本轮结束, 边界前移 */
}

void resetAllNodes(void)
{
    nodeCount = 0;
    dataChanged = false;
    memset(nodes, 0, sizeof(nodes));
}
