/* gateway_oled.h - 网关 OLED 集中显示模块 (0.96寸 SSD1306, I2C)
 *
 * 显示内容:
 *   - 启动阶段画面: 网络连接中 / 服务器连接中 / 节点扫描中
 *   - 配网模式: AP 热点名 + IP (192.168.4.1)
 *   - 运行模式: WiFi/MQTT 状态 + 各节点车位状态轮播
 *   - 节点 >2 个时自动翻页 (每页2个, 5s/页)
 */
#ifndef GATEWAY_OLED_H
#define GATEWAY_OLED_H

#include <Arduino.h>

/* 初始化 OLED (I2C), 失败不影响主流程 */
void oled_init(void);

/* 启动阶段画面 (旧接口, 兼容保留):
 *   phase=1: 网络连接中 (WiFi)
 *   phase=2: 服务器连接中 (MQTT)
 *   phase=3: 节点扫描中 (进入扫描画面, 计时从主循环起算, 退出后进主界面)
 * 新代码推荐用 oled_setBootState() 替代, 支持成功/失败反馈+动画 */
void oled_showStartupPhase(uint8_t phase);

/* Boot 状态机: setup 启动阶段对应一种 OLED 画面 + 动画
 *   BOOT_NONE:         未进入 boot 模式 (走主画面)
 *   BOOT_LOGO:         开机 Logo (静态, 等 setup 切下一阶段)
 *   BOOT_WIFI_CONNECT: WiFi 连接中 (spinner 旋转点动画)
 *   BOOT_WIFI_OK:      WiFi 已连接 (✓ 闪烁 2 次)
 *   BOOT_WIFI_FAIL:    WiFi 失败 (✗ 闪烁 2 次)
 *   BOOT_MQTT_CONNECT: MQTT 连接中 (spinner 旋转点动画)
 *   BOOT_MQTT_OK:      MQTT 已连接 (✓ 闪烁 2 次)
 *   BOOT_MQTT_FAIL:   MQTT 失败 (✗ 闪烁)
 *   BOOT_DONE:         boot 完成, 切回主画面 (走 drawRuntime)
 *
 * 参考 xiaozhi-esp32 设计: 状态切换即刷屏, 每个状态对应独立画面.
 * 临时消息(LoRa is OutTime!)优先级仍最高, boot 画面次之, 主画面最后. */
enum BootState {
    BOOT_NONE = 0,      /* 未进入 boot */
    BOOT_LOGO,          /* 开机 Logo */
    BOOT_WIFI_CONNECT, /* WiFi 连接中 */
    BOOT_WIFI_OK,       /* WiFi 已连接 */
    BOOT_WIFI_FAIL,    /* WiFi 失败 */
    BOOT_MQTT_CONNECT, /* MQTT 连接中 */
    BOOT_MQTT_OK,      /* MQTT 已连接 */
    BOOT_MQTT_FAIL,   /* MQTT 失败 */
    BOOT_DONE          /* 完成, 切主画面 */
};

/* 切换 boot 状态, detail 是辅助信息(SSID/IP/原因), 传 NULL 不显示.
 * 内部立即刷一帧, 不走 oled_refresh 间隔检查, 保证 setup 阶段即时反馈 */
void oled_setBootState(BootState state, const char *detail);

/* 设置 boot 进度条百分比 (0-100), 留作未来扩展 */
void oled_setBootProgress(uint8_t pct);

/* setup 末尾调用: 开启启动扫描节点画面并开始计时 (主循环起算) */
void oled_scanStart(void);

/* 手动搜索 (短按 FLASH) 时调用: 显示搜索节点动画.
 * 与开机扫描不同: 不因"已满节点"立即退出, 至少展示一轮最短时长再回主界面 */
void oled_startManualScan(void);

/* 周期刷新显示 (内部计时, 非阻塞) */
void oled_refresh(void);

/* 显示一行临时消息, 持续 durationMs 毫秒后自动恢复正常画面.
 * 用于调试/告警 (如 LoRa AUX 超时). 非阻塞: 仅记下消息和到期时间,
 * 实际绘制由 oled_refresh() 在周期里完成. OLED 未就绪时安全降级.
 * 消息内容以 ASCII 可打印字符为准, 超过屏宽自动截断. */
void oled_showTempMessage(const char *msg, uint32_t durationMs);

#endif /* GATEWAY_OLED_H */
