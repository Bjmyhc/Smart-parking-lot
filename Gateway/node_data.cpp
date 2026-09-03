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
        nodes[slot].online   = false;    /* 存证不代在线, 等收到 LoRa 应答才改 true */
        memcpy(nodes[slot].productKey, pc.productKey, sizeof(nodes[slot].productKey));
        memcpy(nodes[slot].deviceName, pc.deviceName, sizeof(nodes[slot].deviceName));
        nodes[slot].loginPending = true;  /* 重启后需重新代上线 */
        sysEventFlag |= (1 << (pc.nodeId - 1));
        DBG_PRINTF("[LFS] 加载证书: 节点%d (%s/%s)\n",
                   pc.nodeId, pc.productKey, pc.deviceName);

        /* ⭐ 加载僵尸车阈值: 如有保存则设置待下发标志, 重置重试计数 */
        if (pc.zombieThresholdSec > 0)
        {
            nodes[slot].thresholdValue = pc.zombieThresholdSec;
            nodes[slot].thresholdNeedsUpdate = true;  /* PONG 确认在线后自动下发 */
            nodes[slot].thresholdRetryCount = 0;       /* 重启后重置重试计数 */
            DBG_PRINTF("[LFS] 节点%d 僵尸车阈值=%lu秒 (待PONG下发)\n",
                       pc.nodeId, (unsigned long)pc.zombieThresholdSec);
        }
        /* ⭐ 加载超声波距离阈值: 如有保存则设置待下发标志 */
        if (pc.sensorDistanceCm > 0)
        {
            nodes[slot].sensorDistanceValue = pc.sensorDistanceCm;
            nodes[slot].sensorDistanceNeedsUpdate = true;
            nodes[slot].sensorDistanceRetryCount = 0;
            DBG_PRINTF("[LFS] 节点%d 超声波距离阈值=%ucm (待PONG下发)\n",
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
    nodes[slot].online     = true;
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

    /* ⭐ 仅车位状态变化才触发立即上报; 距离/地磁/LED/阈值等走 15s 定时兜底,
     * 避免距离微小抖动导致每次轮询都上报刷屏平台 */
    bool changed = (nd.parkStatus != parkStatus);

    bool wasOffline = !nd.online;
    nd.parkStatus   = parkStatus;
    nd.geoMagnetic   = (raw->GeoMagnetic != 0);   /* 任意非零值转 0/1 */
    nd.ultrasonic   = ultrasonic;
    nd.occupiedTime = occupiedTime;
    nd.led          = (raw->LED != 0);
    nd.ledEnable    = (raw->LedEnable != 0);
    nd.zombieThresholdSec = raw->ZombieThreshold;   /* ⭐ 节点当前生效阈值, 供 pack/post 上报观看 */
    nd.sensorDistanceCm   = raw->SensorDistanceCm;   /* ⭐ 节点当前生效超声波距离阈值 */
    nd.lastUpdate   = millis();
    nd.online       = true;
    sysEventFlag |= (1 << (nd.nodeId - 1));   /* 同步事件标志位 */

    /* 离线恢复: 若之前已代上线过, 需要重新代上线 */
    if (wasOffline && nd.certSent && !nd.subLogin)
        nd.loginPending = true;

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

    nd.certSent   = true;
    nd.online     = true;
    nd.lastUpdate = millis();     /* 收到证书也算一次 LoRa 活性确认, 防止被超时误判离线 */
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

    /* 证书已更新 -> 持久化到 Flash, 重启后免 LoRa 重新上报 */
    saveCertsToLittleFS();

    /* 同地址换新节点: 打印变更日志 */
    if (certChanged)
        DBG_PRINTF("[节点] 节点%d 证书变更: %s/%s -> %s/%s\n",
                   nodeId, oldPk, oldDn,
                   cert->ProductKey, cert->DeviceName);
}

void checkNodeTimeout(void)
{
    uint32_t now = millis();
    /* 动态超时: 基准 + 已发现节点数 * 每节点附加耗时
     * 节点越多轮询一圈越久, 超时自动放宽避免误判离线;
     * 节点越少超时越短, 离线更快感知 */
    uint32_t timeoutMs = NODE_DATA_TIMEOUT_BASE +
                         (uint32_t)nodeCount * NODE_PER_NODE_TIMEOUT;
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].online && (now - nodes[i].lastUpdate > timeoutMs))
        {
            nodes[i].online = false;
            sysEventFlag &= ~(1 << (nodes[i].nodeId - 1));  /* 清除事件标志位 */
            /* 已代上线过, 需要通知平台子设备离线 */
            if (nodes[i].subLogin)
            {
                nodes[i].logoutPending = true;
                nodes[i].subLogin = false;
            }
            nodes[i].loginPending = false;
            dataChanged = true;
            DBG_PRINTF("[节点] 节点%d 离线 (超时)\n", nodes[i].nodeId);
        }
    }
}

void resetAllNodes(void)
{
    nodeCount = 0;
    dataChanged = false;
    memset(nodes, 0, sizeof(nodes));
}
