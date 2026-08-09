/* node_data.cpp - Gateway 节点数据缓存管理 */
#include "node_data.h"
#include "config.h"
#include <LittleFS.h>

/* ==================== 证书持久化 (LittleFS) ====================
 * 每次收到节点证书后保存到 Flash, 重启后可直接加载, 无需等 LoRa 重新上报 */
#define CERTS_FILE  "/certs.dat"

typedef struct {
    uint8_t  nodeId;               /* 节点ID 1..N */
    char     productKey[12];       /* 子设备产品ID */
    char     deviceName[33];       /* 子设备设备名 */
    bool     certSent;             /* 是否已收到证书 */
} PersistedCert_t;

static const uint8_t CERTS_MAGIC = 0xA5;  /* 文件有效性标记 */

void saveCertsToLittleFS(void)
{
    if (!LittleFS.begin()) return;
    File f = LittleFS.open(CERTS_FILE, "w");
    if (!f) { LittleFS.end(); return; }

    f.write(CERTS_MAGIC);
    f.write(nodeCount);
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
    DBG_PRINTF("[LFS] Saved %d certs\n", nodeCount);
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

        /* 注册节点并填充证书 */
        int slot = findNode(pc.nodeId);
        if (slot < 0) slot = registerNode(pc.nodeId);
        if (slot < 0) continue;

        nodes[slot].certSent = true;
        memcpy(nodes[slot].productKey, pc.productKey, sizeof(nodes[slot].productKey));
        memcpy(nodes[slot].deviceName, pc.deviceName, sizeof(nodes[slot].deviceName));
        nodes[slot].loginPending = true;  /* 加载后重新代上线 */
        sysEventFlag |= (1 << (pc.nodeId - 1));
        DBG_PRINTF("[LFS] Loaded cert: node%d (%s/%s)\n",
                   pc.nodeId, pc.productKey, pc.deviceName);
    }
    f.close();
    LittleFS.end();
    dataChanged = true;
    DBG_PRINTF("[LFS] Loaded %d certs from flash\n", count);
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
    DBG_PRINTF("[Node] Registered node%d (slot %d, total %d)\n",
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

    /* 离线恢复: 若之前已上线过但掉线, 需要重新代上线 */
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
    nd.certSent = true;
    nd.online   = true;
    nd.subLogin = false;          /* 证书更新后需要重新代上线 */
    strncpy(nd.productKey, cert->ProductKey, sizeof(nd.productKey) - 1);
    nd.productKey[sizeof(nd.productKey) - 1] = '\0';
    strncpy(nd.deviceName, cert->DeviceName, sizeof(nd.deviceName) - 1);
    nd.deviceName[sizeof(nd.deviceName) - 1] = '\0';

    /* 证书有效且身份齐全才发起代上线 */
    nd.loginPending = (cert->valid != 0) &&
                      (nd.productKey[0] != '\0') &&
                      (nd.deviceName[0] != '\0');
    nd.logoutPending = false;
    dataChanged = true;

    /* 证书已更新 → 持久化到 Flash, 重启后无需等 LoRa 重新上报 */
    saveCertsToLittleFS();
}

void checkNodeTimeout(void)
{
    uint32_t now = millis();
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].online && (now - nodes[i].lastUpdate > NODE_DATA_TIMEOUT))
        {
            nodes[i].online = false;
            sysEventFlag &= ~(1 << (nodes[i].nodeId - 1));  /* 清除事件标志位 */
            /* 已代上线过, 需要通知平台子设备下线 */
            if (nodes[i].subLogin)
            {
                nodes[i].logoutPending = true;
                nodes[i].subLogin = false;
            }
            nodes[i].loginPending = false;
            dataChanged = true;
            DBG_PRINTF("[Node] Node%d offline (timeout)\n", nodes[i].nodeId);
        }
    }
}

void resetAllNodes(void)
{
    nodeCount = 0;
    dataChanged = false;
    memset(nodes, 0, sizeof(nodes));
}
