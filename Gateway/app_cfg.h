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

/* 搜索模式(开机/短按FLASH)每地址 PING 总尝试次数:
 * LoRa 首帧易丢, 一轮只 PING 一次常因首帧丢失误判"不在线",
 * 多试几次可显著提高搜索阶段发现成功率 (代价: 搜索时长×尝试次数).
 * 注意: 该值是"总尝试次数"(含首次), 例如 3 = 首次 PING + 2 次超时重试 */
#define DISCOVER_PING_ATTEMPTS      3

/* 离线节点探测退避档位数: 正常轮询对"已注册离线节点"的 PING 探测,
 * 每超时一次档位+1, 探测间隔逐档拉大(间隔表见 lora_handler.cpp:
 * 2s→5s→10s→20s→30s→60s 封顶), 防止坏节点长期空耗空口/拖慢轮询;
 * 节点恢复上线(收到任何有效帧)立即回到快节奏 */
#define OFFLINE_BACKOFF_STAGES      6

/* 正常轮询周期(ms): 每轮(所有节点扫一遍)至少间隔这么久才发下一轮.
 * 防止节点响应快时网关"话痨式"连发占满 LoRa 空口(半双工共享信道易撞包).
 * 调小更实时(空口更忙), 调大更省电/更清净. 搜索模式不受此限制
 * (探测节奏由 PING 超时 500ms 自然控制). */
#define LORA_POLL_ROUND_MS       2000

/* ==================== OLED 显示行为 ==================== */
#define OLED_REFRESH_MS     2000    /* 刷新周期(ms) */
#define OLED_ANIM_MS        450     /* 重连动画刷新间隔(ms): WiFi信号条/MQTT圆圈 */

/* OLED 启动扫描节点画面 (第三屏 SEARCHING NODES):
 * 退出条件: 扫描真实结束(discoveryMode 清除) 且 已展示满 OLED_SCAN_MIN_MS.
 * 最短展示时长是"让用户看清搜索过程与 FOUND 数字"的人为保证:
 * 搜索(2个地址)约 1.6s 即结束, 若最短展示太短会一眨眼跳进主界面,
 * 且 FOUND 数字由快档刷新(OLED_ANIM_MS)实时跳动, 3s 足够看完整过程.
 * OLED_SCAN_MAX_MS 是保险: 正常情况下扫描一轮必会结束(节点被动应答,
 * 每个地址要么回包要么超时, 扫完即清 discoveryMode), 这个上限几乎用不到,
 * 仅防止意外情况画面卡死. */
#define OLED_SCAN_MIN_MS        3000    /* 扫描画面最短显示时间(ms): 让用户看清搜索过程与FOUND数字 */
#define OLED_SCAN_MAX_MS        10000   /* 扫描画面最长显示时间(ms, 保险) */

/* ==================== 上报与超时 ==================== */
#define UPLOAD_INTERVAL     5000    /* 定时上报周期(ms): 比赛演示要快, 5s 一推 */
#define UPLOAD_MIN_INTERVAL_MS  1000  /* 上行最小间隔(ms): 配合平台"≤1次/s"限速, 锁死不超 */
/* ⭐ 节点离线判定超时(ms): 实际离线门槛在 node_data.cpp checkNodeTimeout 中按
 * 本动态值 ×2 执行(≈12s/单节点), 用于容忍 LoRa 偶发单帧丢包, 避免误判活节点离线 */
#define NODE_DATA_TIMEOUT_BASE  3000   /* 节点超时基准(ms) */
#define NODE_PER_NODE_TIMEOUT   3000    /* 每发现1个节点附加超时(ms), 适配轮询一圈耗时 */
#define MQTT_RETRY_DELAY    5000    /* MQTT重连间隔(ms) */
#define WIFI_RETRY_DELAY    2000    /* WiFi重连节流(ms) */
#define WIFI_CONNECT_TIMEOUT_MS  10000  /* 一次 begin() 连接等待超时(ms), 超时未连上重新发起 */

/* ==================== 系统事件标志位 ====================
 * 借鉴参考项目 main.h 的 32 位标志位设计, O(1) 判断系统状态.
 * 节点在线位使用动态位 (1 << (nodeId-1)), 由 node_data.cpp 维护.
 * MQTT 保活不占标志位: 底层由 PubSubClient keepalive 自动 PINGREQ
 * (30s) + 60s 无数据自动断开, 连接标志由 onenet_handler 收口维护 */
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
