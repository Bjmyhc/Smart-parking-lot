/* app_cfg.h - 应用行为/调度参数配置
 *
 * 调优运行行为时才需要修改本文件:
 *   - LoRa 轮询/发现/证书校验调度
 *   - 上报/心跳/超时/重连时序
 *   - OLED 刷新与扫描画面时长
 *   - 系统事件标志位 + 调试输出宏
 *
 * 依赖: hw_cfg.h (DBG_* 需要 DEBUG_SERIAL)
 */
#ifndef APP_CFG_H
#define APP_CFG_H

#include "hw_cfg.h"

/* ==================== 网关/节点地址 & 轮询参数 ==================== */
/* 协议常量 (LORA_GATEWAY_ADDR/帧头/结构体) 见 lora_protocol.h */

/* 最大支持节点数 (节点数组大小/轮询范围上限), 接更多节点时改大即可 */
#define LORA_MAX_NODES                 2

/* 单节点响应超时(ms): 发 AT 命令后等回复的最长时间 */
#define LORA_RESPONSE_TIMEOUT_MS       3000

/* 节点发现: 首次启动后, 网关轮询 AT+CER1~CER<MAX>
 * 收到回复后标记"已注册", 后续改轮询 AT+DATAx
 * POLL_FROM 一般从 1 开始, POLL_TO 按需配置 */
#define LORA_POLL_FROM_NODE         1
#define LORA_POLL_TO_NODE           LORA_MAX_NODES  /* 最大搜索 1..16 号 */

/* 证书周期性校验: 已注册节点每 LORA_CERT_VERIFY_EVERY 次数据轮询,
 * 夹发一次 AT+CER 校验证书. 用于发现"同地址换了新节点"(设备名变化),
 * 自动更新证书并重新代上线. 调大省空口, 调小发现更及时. */
#define LORA_CERT_VERIFY_EVERY      50

/* ==================== OLED 显示行为 ==================== */
#define OLED_REFRESH_MS     2000    /* 刷新周期(ms) */
#define OLED_ANIM_MS        450     /* 重连动画刷新间隔(ms): WiFi信号条/MQTT圆圈 */

/* OLED 启动扫描节点画面 (居中显示, 初始化时展示):
 * 部分找到节点时的最短显示时间 = 预期节点数 × NODE_PER_NODE_TIMEOUT
 * (与节点超时共用"每节点耗时"口径, 1 节点约 3s, 2 节点约 6s);
 * 全部找到立即退出; 一个都没找到时 OLED_SCAN_MAX_MS 兜底退出 */
#define OLED_SCAN_MAX_MS        20000   /* 扫描画面最长显示时间(ms) */

/* ==================== 上报与超时 ==================== */
#define UPLOAD_INTERVAL     15000   /* 定时上报周期(ms) */
#define NODE_DATA_TIMEOUT_BASE  3000   /* 节点超时基准(ms) */
#define NODE_PER_NODE_TIMEOUT   3000    /* 每发现1个节点附加超时(ms), 适配轮询一圈耗时 */
#define MQTT_RETRY_DELAY    5000    /* MQTT重连间隔(ms) */
#define WIFI_RETRY_DELAY    2000    /* WiFi重连节流(ms) */
#define WIFI_CONNECT_TIMEOUT_MS  10000  /* 一次 begin() 连接等待超时(ms), 超时未连上重新发起 */

/* ==================== 系统事件标志位 ====================
 * 借鉴参考项目 main.h 的 32 位标志位设计, O(1) 判断系统状态.
 * 节点在线位使用动态位 (1 << (nodeId-1)), 由 node_data.cpp 维护.
 * MQTT 保活不占标志位: 底层由 PubSubClient keepalive 自动 PINGREQ
 * (60s) + 120s 无数据自动断开, 连接标志由 onenet_handler 收口维护 */
#define SYS_EVENT_MQTT_CONNECTED    0x80000000  /* MQTT 已连接(CONNACK收到) */
#define SYS_EVENT_CONFIG_PORTAL     0x20000000  /* AP 配网模式 */
/* ... 0x1F000000 及低 16 位按需扩展 (低16位每位对应一个节点) */

/* 外部声明 */
extern uint32_t sysEventFlag;

/* ==================== 调试输出 ==================== */
#define DEBUG_PRINT         1       /* 1=启用, 0=关闭 */

#if DEBUG_PRINT
  #define DBG_PRINT(x)      DEBUG_SERIAL.print(x)
  #define DBG_PRINTLN(x)    DEBUG_SERIAL.println(x)
  #define DBG_PRINTF(f, ...) DEBUG_SERIAL.printf(f, ##__VA_ARGS__)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(f, ...)
#endif

#endif /* APP_CFG_H */
