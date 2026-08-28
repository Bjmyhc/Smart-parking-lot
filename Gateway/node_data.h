/* node_data.h - 节点数据管理 (Gateway 端)
 * 字段 LoraNodeData_t 必须与 lora_protocol.h / STM32 端 lora_node.h 一致 */
#ifndef NODE_DATA_H
#define NODE_DATA_H

#include <Arduino.h>
#include "app_cfg.h"      /* LORA_MAX_NODES */
#include "lora_protocol.h"

/* 节点状态缓存 */
struct NodeData {
    uint8_t  nodeId;        /* 1..LORA_MAX_NODES */
    uint8_t  parkStatus;    /* 0=空闲, 1=有车, 2=僵尸占用 */
    uint8_t  geoMagnetic;    /* 0/1 */
    uint16_t ultrasonic;    /* cm */
    uint32_t occupiedTime;  /* s */
    bool     led;           /* LED当前状态 */
    bool     ledEnable;     /* LED使能开关 */
    char     fwVersion[16]; /* 节点固件版本字符串(如 "v2.321"), 节点上报帧携带 */
    uint32_t lastUpdate;    /* 最后收到数据时间(ms) */
    bool     certSent;      /* 是否已收到证书 */
    bool     online;        /* LoRa 在线/离线 */

    /* ---- 子设备证书与 OneNET 代上线状态 (来自 LoraNodeCert_t) ---- */
    char     productKey[12];  /* 子设备产品ID */
    char     deviceName[33];  /* 子设备设备名(park1/park2...) */
    bool     loginPending;    /* 已拿到证书, 待代上线 */
    bool     logoutPending;   /* 节点离线, 待代下线 */
    bool     subLogin;        /* 已代子设备上线成功 */
    bool     thresholdNeedsUpdate; /* ⭐ 僵尸车判定阈值待下发(收到MQTT配置后标记, PONG确认在线后下发) */
    uint32_t thresholdValue;      /* ⭐ 待下发的阈值(秒), 0=使用默认 */
    uint8_t  thresholdRetryCount;  /* ⭐ 下发重试次数, 超过3次放弃 (防止无限重试阻塞) */
    uint32_t zombieThresholdSec;   /* ⭐ 节点上报的当前生效阈值(秒), 随 pack/post 上报只读属性观看 */
};

extern NodeData nodes[LORA_MAX_NODES];
extern uint8_t  nodeCount;      /* 已发现节点数 */
extern bool     dataChanged;    /* 任何节点数据变化, 触发立即上报 */

/* 查找索引, 未找到返回 -1 */
int findNode(uint8_t nodeId);

/* 注册新节点, 返回索引 */
int registerNode(uint8_t nodeId);

/* 从 LoraNodeData_t (二进制接收) 更新节点数据 */
void updateNodeFromRaw(uint8_t nodeId, const LoraNodeData_t *raw);

/* 从 LoraNodeCert_t (二进制接收) 更新子设备证书, 并标记待代上线 */
void updateNodeCert(uint8_t nodeId, const LoraNodeCert_t *cert);

/* 超时时标记为离线 */
void checkNodeTimeout(void);

/* 清空所有 */
void resetAllNodes(void);

/* 证书持久化 (LittleFS): 保存到 Flash / 重启后加载 */
void saveCertsToLittleFS(void);
void loadCertsFromLittleFS(void);

#endif /* NODE_DATA_H */
