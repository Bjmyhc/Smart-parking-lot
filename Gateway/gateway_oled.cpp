/* gateway_oled.cpp - 网关 OLED 集中显示模块实现
 *
 * 依赖库 (Arduino IDE 库管理器安装):
 *   - Adafruit SSD1306
 *   - Adafruit GFX Library
 *   - Adafruit BusIO (SSD1306 自动依赖)
 *
 * 字体: 内嵌 8x8 位图字库 (U+0020..U+007E, Public Domain, Daniel Hepper font8x8)
 *       Adafruit 自带只有 5x7 字库, 8x8 大字需自带字库
 *       ASCII 可打印字符全部内置, 均按位图绘制
 *
 * 布局 (128x64, 双色屏: 上 1/4 高 16px 黄 / 下 3/4 高 48px 蓝):
 *   标题 : PGW001 [WiFi条] MQTT●           <- Header 独占 (黄色区)
 *   节点 : #01 OCC  [信号] 12s           <- 节点行(每页4个, 异常反显)
 *        : #02 EMP  [信号] 3s
 *        : #03 ERR  [信号] -- (离线)
 *        : #04 OCC  [信号] 8s
 *        : 上42 下9  P1/3                   <- Footer
 */
#include "gateway_oled.h"
#include "platform_cfg.h"  /* CONFIG_AP_SSID/ONENET_DEVID (Header/配网画面) */
#include "hw_cfg.h"        /* OLED 引脚/I2C 地址 */
#include "app_cfg.h"       /* OLED_REFRESH_MS/SCAN_MAX_MS/sysEventFlag/DBG */
#include "node_data.h"
#if defined(ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <stdarg.h>

/* 上下行 MQTT 消息计数 (定义在 onenet_handler.cpp) */
extern uint32_t mqttTxCount;
extern uint32_t mqttRxCount;

/* 128x64, 无复位引脚 */
static Adafruit_SSD1306 display(128, 64, &Wire, -1);

static uint32_t lastRefresh  = 0;
static uint8_t  pageIdx      = 0;
static uint32_t lastPageFlip = 0;
static bool     displayReady = false;
/* 0=normal, 1=wifi, 2=mqtt, 3=scan */
static uint8_t  startupPhase = 0;
/* 启动扫描节点画面: 初始化时全屏居中显示, 找到节点后自动切换到主界面 */
static bool     scanPhase    = true;
static uint32_t scanStartMs  = 0;

/* ==================== 8x8 ASCII 位图字库 ====================
 * 用法: font8x8[c - 0x20], c 为 ASCII 32..126
 * Public Domain, 来源: https://github.com/dhepper/font8x8 (IBM VGA 8x8)
 *
 * 注意: 不能放 PROGMEM! 实测在 ESP8266 core/flash 访问优化下,
 *       直接 [] 访问 .irom.text 段的 PROGMEM 数据会触发
 *       Exception(3) LoadStoreError 导致无限重启.
 *       760 字节放 RAM 毫无压力, 完全稳定. */
static const uint8_t font8x8[95][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x20 space */
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 }, /* 0x21 ! */
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x22 " */
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 }, /* 0x23 # */
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00 }, /* 0x24 $ */
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00 }, /* 0x25 % */
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00 }, /* 0x26 & */
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x27 ' */
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00 }, /* 0x28 ( */
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00 }, /* 0x29 ) */
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, /* 0x2A * */
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00 }, /* 0x2B + */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, /* 0x2C , */
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00 }, /* 0x2D - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, /* 0x2E . */
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00 }, /* 0x2F / */
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00 }, /* 0x30 0 */
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00 }, /* 0x31 1 */
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00 }, /* 0x32 2 */
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00 }, /* 0x33 3 */
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00 }, /* 0x34 4 */
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00 }, /* 0x35 5 */
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00 }, /* 0x36 6 */
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00 }, /* 0x37 7 */
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00 }, /* 0x38 8 */
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00 }, /* 0x39 9 */
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, /* 0x3A : */
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, /* 0x3B ; */
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00 }, /* 0x3C < */
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00 }, /* 0x3D = */
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00 }, /* 0x3E > */
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00 }, /* 0x3F ? */
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00 }, /* 0x40 @ */
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00 }, /* 0x41 A */
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00 }, /* 0x42 B */
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00 }, /* 0x43 C */
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00 }, /* 0x44 D */
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00 }, /* 0x45 E */
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00 }, /* 0x46 F */
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00 }, /* 0x47 G */
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00 }, /* 0x48 H */
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, /* 0x49 I */
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00 }, /* 0x4A J */
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00 }, /* 0x4B K */
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00 }, /* 0x4C L */
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00 }, /* 0x4D M */
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00 }, /* 0x4E N */
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00 }, /* 0x4F O */
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00 }, /* 0x50 P */
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00 }, /* 0x51 Q */
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00 }, /* 0x52 R */
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00 }, /* 0x53 S */
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, /* 0x54 T */
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00 }, /* 0x55 U */
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, /* 0x56 V */
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, /* 0x57 W */
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00 }, /* 0x58 X */
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00 }, /* 0x59 Y */
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00 }, /* 0x5A Z */
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00 }, /* 0x5B [ */
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 }, /* 0x5C \ */
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00 }, /* 0x5D ] */
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00 }, /* 0x5E ^ */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF }, /* 0x5F _ */
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x60 ` */
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00 }, /* 0x61 a */
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00 }, /* 0x62 b */
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00 }, /* 0x63 c */
    { 0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00 }, /* 0x64 d */
    { 0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00 }, /* 0x65 e */
    { 0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00 }, /* 0x66 f */
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F }, /* 0x67 g */
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00 }, /* 0x68 h */
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, /* 0x69 i */
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E }, /* 0x6A j */
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00 }, /* 0x6B k */
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, /* 0x6C l */
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00 }, /* 0x6D m */
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00 }, /* 0x6E n */
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00 }, /* 0x6F o */
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F }, /* 0x70 p */
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78 }, /* 0x71 q */
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00 }, /* 0x72 r */
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00 }, /* 0x73 s */
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00 }, /* 0x74 t */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00 }, /* 0x75 u */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, /* 0x76 v */
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00 }, /* 0x77 w */
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00 }, /* 0x78 x */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F }, /* 0x79 y */
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00 }, /* 0x7A z */
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00 }, /* 0x7B { */
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 }, /* 0x7C | */
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00 }, /* 0x7D } */
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x7E ~ */
};

/* 8x8 字符打印: 在 (x,y) 绘制字符串, 每字符 8x8px, 指定颜色(可反色)
 * 注意: 本字库每字节 bit0 对应最左列 (LSB 在前),
 *       不像 Adafruit 自带的 drawBitmap (bit7 最左), 否则字符会镜像,
 *       所以必须用 drawPixel 按 1<<col 方式逐像素绘制 */
static void oled8x8Print(int16_t x, int16_t y, const char *s, uint16_t color)
{
    for (; *s; s++)
    {
        uint8_t c = (uint8_t)*s;
        if (c < 0x20 || c > 0x7E) c = '?';   /* 不可打印 -> 问号 */
        for (uint8_t row = 0; row < 8; row++)
        {
            uint8_t bits = font8x8[c - 0x20][row];
            for (uint8_t col = 0; col < 8; col++)
            {
                if (bits & (1 << col))
                    display.drawPixel(x + col, y + row, color);
            }
        }
        x += 8;
    }
}

/* 8x8 格式化打印 */
static void oled8x8Printf(int16_t x, int16_t y, uint16_t color, const char *fmt, ...)
{
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    oled8x8Print(x, y, buf, color);
}

/* ==================== 图形元素绘制 ==================== */

/* 信号条: 4 根递增柱状 (2/4/6/8px), level 0..4 */
static void drawBars(int16_t x, int16_t y, uint8_t level, uint16_t color)
{
    if (level > 4) level = 4;
    for (uint8_t i = 0; i < 4; i++)
    {
        uint8_t h = 2 + i * 2;
        if (i < level)
            display.fillRect(x + i * 2, y + 8 - h, 2, h, color);
    }
}

/* 状态圆点: on=实心, off=空心 */
static void drawStatusDot(int16_t x, int16_t y, bool on, uint16_t color)
{
    if (on)
        display.fillCircle(x + 3, y + 3, 3, color);
    else
        display.drawCircle(x + 3, y + 3, 3, color);
}

/* 状态圆点动画: 圆环 + 亮点绕圆周转动 (MQTT 重连中) */
static void drawStatusSpinner(int16_t x, int16_t y, uint16_t color)
{
    display.drawCircle(x + 3, y + 3, 3, color);
    static const int8_t rot[4][2] = { {3, 0}, {0, 3}, {-3, 0}, {0, -3} };
    uint8_t p = (millis() / OLED_ANIM_MS) % 4;
    display.drawPixel(x + 3 + rot[p][0], y + 3 + rot[p][1], color);
}

/* 上箭头 ▲ */
static void drawArrowUp(int16_t x, int16_t y, uint16_t color)
{
    display.drawLine(x + 3, y + 7, x + 3, y + 1, color);
    display.drawLine(x + 3, y + 1, x + 1, y + 3, color);
    display.drawLine(x + 3, y + 1, x + 5, y + 3, color);
}

/* 下箭头 ▼ */
static void drawArrowDown(int16_t x, int16_t y, uint16_t color)
{
    display.drawLine(x + 3, y + 1, x + 3, y + 7, color);
    display.drawLine(x + 3, y + 7, x + 1, y + 5, color);
    display.drawLine(x + 3, y + 7, x + 5, y + 5, color);
}

/* ==================== 状态相关函数 ==================== */

/* 车位状态 4 字符 (在线时) */
static const char *stateStr(uint8_t s)
{
    switch (s)
    {
        case 0:  return "EMP ";   /* 空闲 */
        case 1:  return "OCC ";   /* 有车 */
        case 2:  return "ZOMB";   /* 僵尸车 */
        default: return "??? ";
    }
}

/* WiFi 信号等级 0..4 (基于 RSSI) */
static uint8_t wifiLevel(void)
{
    if (WiFi.status() != WL_CONNECTED) return 0;
    int32_t rssi = WiFi.RSSI();
    if (rssi > -55) return 4;
    if (rssi > -67) return 3;
    if (rssi > -75) return 2;
    return 1;
}

/* 节点"信号"等级 0..4: LoRa 帧无 RSSI 字段, 用距上次刷新时长模拟 */
static uint8_t sigLevel(const NodeData &nd)
{
    if (!nd.online) return 0;
    uint32_t age = (uint32_t)(millis() - nd.lastUpdate) / 1000;
    if (age < 10) return 4;
    if (age < 20) return 3;
    if (age < 40) return 2;
    return 1;
}

/* 当前确实在线的节点数: 只统计真正收到过 LoRa 应答的节点为"在线".
 * Flash 里存的证书只说明设备存在, 不能说明在线, 不计入统计 */
static uint8_t liveNodeCount(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < nodeCount; i++)
        if (nodes[i].online) n++;
    return n;
}

/* 预期扫描的节点数量 (轮询地址范围, 即扫描总数的上界) */
static uint8_t scanExpected(void)
{
    return LORA_POLL_TO_NODE - LORA_POLL_FROM_NODE + 1;
}

/* 启动扫描节点画面: 全屏居中, 动画圆点 + 已发现节点数 + 提示 */
static void drawScanScreen(void)
{
    display.clearDisplay();

    /* 与阶段1/2 风格统一: 顶部 STEP 3/3 */
    oled8x8Print((128 - 8 * 8) / 2, 8, "STEP 3/3", SSD1306_WHITE);

    /* 标题 + 动画点 (0.4s 轮换): SCANNING NODES / . / .. / ... */
    static const char dot[4][4] = { "", ".", "..", "..." };
    uint8_t d = (millis() / 400) % 4;
    char title[24];
    snprintf(title, sizeof(title), "SCANNING NODES%s", dot[d]);
    oled8x8Print((128 - (int16_t)strlen(title) * 8) / 2, 20, title, SSD1306_WHITE);

    /* 已发现(在线)节点数 / 预期总数, 居中显示 */
    char found[16];
    snprintf(found, sizeof(found), "FOUND: %u/%u", liveNodeCount(), scanExpected());
    oled8x8Print((128 - (int16_t)strlen(found) * 8) / 2, 30, found, SSD1306_WHITE);

    display.display();
}

/* startup screen: centered "STEP x/3" + description
 * phase 1=network(wifi) 2=server(mqtt); phase 3 uses drawScanScreen() */
static void drawStartupScreen(uint8_t phase)
{
    display.clearDisplay();

    char title[16];
    snprintf(title, sizeof(title), "STEP %u/3", phase);
    oled8x8Print((128 - (int16_t)strlen(title) * 8) / 2, 18, title, SSD1306_WHITE);

    static const char *desc[2] = { "NETWORK CONNECT", "SERVER CONNECT" };
    const char *d = (phase >= 1 && phase <= 2) ? desc[phase - 1] : "";
    oled8x8Print((128 - (int16_t)strlen(d) * 8) / 2, 30, d, SSD1306_WHITE);

    display.display();
}

/* switch startup phase (setup calls in order): 1=wifi 2=mqtt 3=scan */
void oled_showStartupPhase(uint8_t phase)
{
    startupPhase = phase;
    if (phase == 3)
    {
        oled_scanStart();
    }
    else
    {
        drawStartupScreen(phase);
    }
}

void oled_init(void)
{
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    delay(50);

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR))
    {
        DBG_PRINTLN("[OLED] Init failed (no display)");
        return;
    }
    displayReady = true;

    /* 默认显示阶段1(网络连接中), 实际阶段由 setup 调 oled_showStartupPhase 切换 */
    scanPhase = true;
    startupPhase = 1;
    drawStartupScreen(1);
    DBG_PRINTLN("[OLED] Init OK");
}

/* setup 末尾调用: 扫描画面计时从主循环开始, 避免 setup 里
 * WiFi 阻塞等待(最长15s)把 8s/20s 的扫描时长耗尽 */
void oled_scanStart(void)
{
    scanStartMs = millis();
    scanPhase   = true;
}

/* 配网模式显示: AP 名 + IP */
static void drawConfigPortal(void)
{
    display.clearDisplay();
    oled8x8Printf(0, 0, SSD1306_WHITE, "Config Mode");
    oled8x8Printf(0, 8, SSD1306_WHITE, "AP:%.10s", CONFIG_AP_SSID);
    oled8x8Printf(0, 16, SSD1306_WHITE, "IP: 192.168.4.1");
    oled8x8Printf(0, 24, SSD1306_WHITE, "Open any URL");
    display.display();
}

/* Header: 设备名+WiFi信号条 / MQTT点状态(右对齐)
 * 屏幕为双色屏: 上 1/4(0..15px) 黄色, 下 3/4(16..63px) 蓝色.
 * Header 独占黄色区 (y=4 垂直居中), 节点行/Footer 全在蓝色区
 * 节点在线状态由下方节点行体现, Header 不再重复显示 LoRa 点 */
static void drawHeader(void)
{
    oled8x8Print(0, 4, ONENET_DEVID, SSD1306_WHITE);          /* 设备名 PGW001 */

    /* WiFi 信号条: 已连画实际等级; 重连中逐格跳动(1→2→3→4 循环) */
    if (WiFi.status() == WL_CONNECTED)
        drawBars(56, 4, wifiLevel(), SSD1306_WHITE);
    else
        drawBars(56, 4, (millis() / OLED_ANIM_MS) % 4 + 1, SSD1306_WHITE);

    oled8x8Print(88, 4, "MQTT", SSD1306_WHITE);               /* MQTT 状态(右对齐) */

    /* MQTT 状态点: 已连=实心; WiFi在线但MQTT重连中=转动动画; 全断=空心 */
    if (sysEventFlag & SYS_EVENT_MQTT_CONNECTED)
        drawStatusDot(120, 4, true, SSD1306_WHITE);
    else if (WiFi.status() == WL_CONNECTED)
        drawStatusSpinner(120, 4, SSD1306_WHITE);
    else
        drawStatusDot(120, 4, false, SSD1306_WHITE);
}

/* Footer: 上/下计数 + 页码 */
static void drawFooter(uint8_t page, uint8_t pages)
{
    /* 计数固定 2 位 (超 99 显示 99), 保证左右对齐 */
    uint32_t tx = (mqttTxCount > 99) ? 99 : mqttTxCount;
    uint32_t rx = (mqttRxCount > 99) ? 99 : mqttRxCount;

    drawArrowUp(0, 56, SSD1306_WHITE);
    oled8x8Printf(8, 56, SSD1306_WHITE, "%02lu", tx);
    drawArrowDown(24, 56, SSD1306_WHITE);
    oled8x8Printf(32, 56, SSD1306_WHITE, "%02lu", rx);
    oled8x8Printf(96, 56, SSD1306_WHITE, "P%d/%d", page, pages);
}

/* 运行模式显示 (每页4个节点, 节点行: 编号+状态+信号+时长, 异常反显) */
static void drawRuntime(void)
{
    display.clearDisplay();

    drawHeader();
    display.drawFastHLine(0, 16, 128, SSD1306_WHITE);  /* 黄/蓝分界横线 */

    const uint8_t per = 4;
    uint8_t pages = (nodeCount + per - 1) / per;
    if (pages == 0) pages = 1;
    if (pageIdx >= pages) pageIdx = 0;

    for (uint8_t i = 0; i < per; i++)
    {
        uint8_t slot = pageIdx * per + i;
        int16_t y = 18 + i * 9;                        /* 每行 9px, 全在蓝色区 */
        if (slot >= nodeCount)
        {
            oled8x8Printf(0, y, SSD1306_WHITE, "#--");
            continue;
        }

        NodeData &nd = nodes[slot];
        bool err = !nd.online;
        uint16_t col = err ? SSD1306_BLACK : SSD1306_WHITE;

        /* 异常反显: 整行白底 + 黑字 */
        if (err)
            display.fillRect(0, y - 1, 128, 9, SSD1306_WHITE);

        oled8x8Printf(0, y, col, "#%02d", nd.nodeId);            /* 编号 */
        oled8x8Print(32, y, err ? "ERR " : stateStr(nd.parkStatus), col); /* 状态 */
        drawBars(88, y, sigLevel(nd), col);                      /* 信号(右对齐) */
        if (err)                                                 /* 停车时长(右对齐) */
            oled8x8Print(104, y, "--", col);   /* 离线: 无停车时长 */
        else if (nd.occupiedTime < 60)
            oled8x8Printf(104, y, col, "%lus", (unsigned long)nd.occupiedTime);
        else if (nd.occupiedTime < 3600)
            oled8x8Printf(104, y, col, "%lum", (unsigned long)(nd.occupiedTime / 60));
        else
            oled8x8Printf(104, y, col, "%luh", (unsigned long)(nd.occupiedTime / 3600));
    }

    display.drawFastHLine(0, 54, 128, SSD1306_WHITE);  /* Footer 分隔线 */
    drawFooter(pageIdx + 1, pages);
    display.display();
}

void oled_refresh(void)
{
    if (!displayReady) return;

    uint32_t now = millis();
    /* 重连动画加速: WiFi 或 MQTT 处于重连中时, 用 OLED_ANIM_MS 快速刷新,
     * 让信号条逐格跳动 / 圆圈转动流畅显示; 全部在线才恢复 2s 慢刷 */
    uint32_t interval = OLED_REFRESH_MS;
    if (WiFi.status() != WL_CONNECTED ||
        !(sysEventFlag & SYS_EVENT_MQTT_CONNECTED))
        interval = OLED_ANIM_MS;
    if (now - lastRefresh < interval) return;
    lastRefresh = now;

    /* 配网模式: 显示配网信息 */
    if (sysEventFlag & SYS_EVENT_CONFIG_PORTAL)
    {
        drawConfigPortal();
        return;
    }

    /* startup phases 1/2: network or server connecting */
    if (startupPhase == 1 || startupPhase == 2)
    {
        drawStartupScreen(startupPhase);
        return;
    }

    /* 扫描节点画面: 全屏居中显示, 满足任一条件自动切换到主界面:
     *  a) 已发现(在线)节点数达到预期范围上限
     *  b) 至少发现 1 个节点且已显示时长 >= 预期节点数×NODE_PER_NODE_TIMEOUT
     *  c) 一个都没找到, OLED_SCAN_MAX_MS 兜底退出 */
    if (scanPhase)
    {
        uint8_t found    = liveNodeCount();
        uint32_t elapsed = now - scanStartMs;
        /* 最短显示时长 = 每节点耗时 × 预期节点数, 与节点超时口径统一
         * (默认 NODE_PER_NODE_TIMEOUT, 只约束"部分找到"的停留时间) */
        uint32_t minMs   = (uint32_t)scanExpected() * NODE_PER_NODE_TIMEOUT;
        if (found >= scanExpected() ||
            (found > 0 && elapsed >= minMs) ||
            elapsed >= OLED_SCAN_MAX_MS)
        {
            scanPhase = false;
            startupPhase = 0;   /* scan done, enter main UI */
        }
        else
        {
            drawScanScreen();
            return;
        }
    }

    drawRuntime();

    /* 多节点翻页: 每 5s 翻一页 (每页4个) */
    if (nodeCount > 4)
    {
        if (now - lastPageFlip >= 5000)
        {
            lastPageFlip = now;
            pageIdx++;
        }
    }
}
