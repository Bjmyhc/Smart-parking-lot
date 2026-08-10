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
} PersistedCert_t;

static const uint8_t CERTS_MAGIC = 0xA5;  /* 文件有效性标识 */

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
        f.write((uint8_t *)&pc, sizeof(PersistedCert_t));
    }
    f.close();
    LittleFS.end();
    DBG_PRINTF("[LFS] 已保存 %d 条证书\n", saved);
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
    }
    f.close();
    LittleFS.end();
    dataChanged = true;
    DBG_PRINTF("[LFS] 已从Flash加载 %d 条证书\n", count);
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
    bool changed = (nd.parkStatus != raw->ParkStatus) ||
                   (nd.ultrasonic != raw->Ultrasonic) ||
                   (nd.geoMagnetic != raw->GeoMagnetic) ||
                   (nd.led != (raw->LED != 0)) ||
                   (nd.ledEnable != (raw->LedEnable != 0));

    bool wasOffline = !nd.online;
    nd.parkStatus   = raw->ParkStatus;
    nd.geoMagnetic   = raw->GeoMagnetic;
    nd.ultrasonic   = raw->Ultrasonic;
    nd.occupiedTime = raw->OccupiedTime;
    nd.led          = (raw->LED != 0);
    nd.ledEnable    = (raw->LedEnable != 0);
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
