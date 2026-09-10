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
    bool     led;           /* LED(报警灯)实际状态 */
    bool     ledSwitch;     /* ⭐ SetLed 服务最近一次命令目标值(0/1), LoRa ACK 后回 ActualValue */
    int8_t   rssi;          /* ⭐ 本次接收节点数据帧的信号强度(dBm, -120~0), 来自模块DRSSI附加字节 */
    char     fwVersion[16]; /* 节点固件版本字符串(如 "v2.321"), 节点上报帧携带 */
    uint32_t lastUpdate;    /* 最后收到数据时间(ms) */
    bool     certSent;      /* 是否已收到证书 */

    /* ---- 三态正交状态 (S9: 替代原单一 online, 只经 updateNodeState 修改) ----
     * linkAlive    链路存活: 最近收到过任何有效响应 (PONG/DATA/CERT)
     * mode         节点固件模式: NODE_MODE_UNKNOWN/APP/BOOT (由 PONG,<MODE> 或数据帧确定)
     * serviceOnline 业务在线: mode==APP 且有业务数据; 平台/运维看到的"在线"统一用此字段
     * dataMissCount 快速路径 DATA/CER 连续无响应计数, 达 DATA_MISS_OFFLINE_MAX 自动降级 PING */
    bool     linkAlive;
    uint8_t  mode;
    bool     serviceOnline;
    uint8_t  dataMissCount;
    /* ⭐ S13: 连续"轮询轮次"无响应计数. 每扫完一轮(advanceNextNode 回绕)
     * 由 nodeRoundCompleted 统计: 本轮有响应清零, 无响应+1,
     * 达 NODE_MISSED_ROUNDS_MAX 判离线 (替代原墙钟动态超时) */
    uint8_t  missedRounds;
    /* ⭐ S14: 待 OTA 标志. 节点在 Boot(收到 PONG,BOOT)且网关无固件文件时置位,
     * 等待平台下发新固件后立即下发(8.2); 节点确认离开 Boot(mode=APP)或
     * OTA 显式启动时清除 */
    bool     bootPending;
    /* ⭐ S21: 拒绝升级标志. 节点回 AT+OTA:version_ok (App 版本比较发现不更新)
     * 时置位, 终止对 App 的 Xmodem 流; 待人工/平台介入, 人工重触发时清除 */
    bool     otaRefused;

    /* ---- 子设备证书与 OneNET 代上线状态 (来自 LoraNodeCert_t) ---- */
    char     productKey[12];  /* 子设备产品ID */
    char     deviceName[33];  /* 子设备设备名(park1/park2...) */
    bool     loginPending;    /* 已拿到证书, 待代上线 */
    bool     logoutPending;   /* 节点离线, 待代下线 */
    bool     subLogin;        /* 已代子设备上线成功 */
    /* ⭐ S29: 是否收到过节点业务数据帧. 网关只在节点真正上报过数据后
     * 才代子设备上报属性; 上线(PONG)成功但数据帧未到时不报, 避免
     * 上报全 0 垃圾快照污染平台存储 (升级后首帧上报 ZombieThresholdSec=0
     * 被平台 code=2213 拒绝的根因) */
    bool     hasDataFrame;
    bool     thresholdNeedsUpdate; /* ⭐ 僵尸车判定阈值待下发(收到MQTT配置后标记, PONG确认在线后下发) */
    uint32_t thresholdValue;      /* ⭐ 待下发的阈值(秒), 0=使用默认 */
    uint8_t  thresholdRetryCount;  /* ⭐ 下发重试次数, 超过3次放弃 (防止无限重试阻塞) */
    uint32_t zombieThresholdSec;   /* ⭐ 节点上报的当前生效阈值(秒), 随 pack/post 上报只读属性观看 */
    uint16_t sensorDistanceCm;     /* ⭐ 节点上报的当前生效超声波距离阈值(cm), 随 pack/post 上报只读属性 */
    /* ⭐ 超声波判定距离阈值(参考僵尸车阈值机制: PONG后下发, 限次重试) */
    bool     sensorDistanceNeedsUpdate;  /* 待下发 */
    uint16_t sensorDistanceValue;         /* 待下发的距离(cm), 0=使用默认 */
    uint8_t  sensorDistanceRetryCount;    /* 下发重试次数 */
};

/* ---- 三态模式枚举 (S9) ---- */
enum NodeMode {
    NODE_MODE_UNKNOWN = 0,   /* 未确认模式 (新注册/刚加载证书) */
    NODE_MODE_APP     = 1,   /* 运行 App 业务固件 */
    NODE_MODE_BOOT    = 2    /* 运行 BootLoader (等待 OTA) */
};

/* ---- 状态更新事件 (S9: linkAlive/mode/serviceOnline 只经 updateNodeState 修改) ---- */
enum NodeEvent {
    NODE_EVT_PONG_APP,    /* 收到 PONG / PONG,APP: 链路活 + 模式=APP + 业务在线 */
    NODE_EVT_PONG_BOOT,   /* 收到 PONG,BOOT:    链路活 + 模式=BOOT + 业务摘除 */
    NODE_EVT_DATA,        /* 收到业务数据帧:     链路活 + 模式=APP + 业务在线 */
    NODE_EVT_CERT,        /* 收到证书帧:         链路活 + 模式=APP (证书只有 App 上报) */
    NODE_EVT_TIMEOUT,     /* 超时窗口到期:       链路死 + 业务摘除 */
    NODE_EVT_DATA_MISS    /* 快速路径 DATA/CER 超时: 计数+1, 达上限业务摘除转 PING */
};

/* 快速路径连续无响应多少次判定业务失活 (7.2#3) */
#define DATA_MISS_OFFLINE_MAX  2

/* ⭐ S13: 节点连续多少"轮询轮次"无响应判离线.
 * 轮次计天然容忍单帧丢包(下一轮 PING 复核成功即清零),
 * 且不受节点数量/轮询一圈耗时影响, OTA 暂停期(无轮次边界)也不会误判 */
#define NODE_MISSED_ROUNDS_MAX  3

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

/* ⭐ S13: 一轮轮询结束(所有节点扫完一遍, advanceNextNode 回绕)时由
 * lora_handler 调用. 以"轮询轮次"而非墙钟统计超时: 本轮有响应的节点
 * 清零 missedRounds, 无响应的 +1, 连续 NODE_MISSED_ROUNDS_MAX 轮无响应判离线 */
void nodeRoundCompleted(void);

/* ⭐ 三态状态统一赋值入口 (S9): linkAlive/mode/serviceOnline 只经此修改.
 * event 见 NodeEvent; 返回 true 表示本次事件使节点由"链路死"转"链路活"(离线恢复跃迁) */
bool updateNodeState(uint8_t slot, uint8_t event);

/* 清空所有 */
void resetAllNodes(void);

/* 证书持久化 (LittleFS): 保存到 Flash / 重启后加载 */
void saveCertsToLittleFS(void);
void loadCertsFromLittleFS(void);

/* ⭐ S19: 证书/阈值持久化异步化.
 * certsMarkDirty():  证书或阈值内容变更时置脏标记 (替代原同步 saveCertsToLittleFS,
 *                     避免 Flash 擦写在 LoRa 轮询/MQTT 回调链中阻塞时序)
 * persistCertsIfDirty(): 主循环每拍调用, 有脏标记才落盘 (节流: 仅变更后写), 无则零开销 */
void certsMarkDirty(void);
void persistCertsIfDirty(void);

#endif /* NODE_DATA_H */
