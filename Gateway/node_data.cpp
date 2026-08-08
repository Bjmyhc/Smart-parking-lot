/* node_data.cpp - Gateway 节点数据缓存管理 */
#include "node_data.h"
#include "config.h"

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
}

void checkNodeTimeout(void)
{
    uint32_t now = millis();
    for (uint8_t i = 0; i < nodeCount; i++)
    {
        if (nodes[i].online && (now - nodes[i].lastUpdate > NODE_DATA_TIMEOUT))
        {
            nodes[i].online = false;
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
