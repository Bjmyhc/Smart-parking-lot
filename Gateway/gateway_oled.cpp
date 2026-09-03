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
#include "lora_handler.h"  /* lora_discoveryActive(): 判断扫描是否真实结束 */
#if defined(ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <stdarg.h>
#include <string.h>   /* strncpy/strlen (临时消息缓冲) */
#include <math.h>     /* cosf/sinf (Spinner 旋转动画) */

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

/* ==================== Boot 状态机 (新, 替代 startupPhase 1/2) ====================
 * 参考 xiaozhi-esp32 设计: 每个状态对应独立 OLED 画面 + 动画
 *   BOOT_LOGO / WIFI_CONNECT / WIFI_OK / WIFI_FAIL
 *   MQTT_CONNECT / MQTT_OK / MQTT_FAIL / DONE
 * 节点扫描(原 startupPhase=3)仍走 scanPhase/drawScanScreen 旧逻辑, 不变 */
static BootState bootState       = BOOT_NONE;
static uint32_t  bootStateMs    = 0;     /* 进入当前状态的时刻 */
static char      bootDetail[24] = {0};   /* 辅助信息: SSID/IP/原因 */
static uint8_t   bootProgress    = 0;    /* 进度条百分比 (0-100) */
static uint32_t  bootLastFrameMs = 0;    /* 动画帧上次时间 */
static uint8_t   bootFrame       = 0;    /* 动画帧序号 */

/* 临时消息(调试/告警): 调用方设置消息和到期时间, refresh 周期内绘制;
 * 到期自动清空, 恢复正常画面. 非阻塞, OLED 未就绪时安全降级 */
static char     tempMsgBuf[24] = {0};
static uint32_t tempMsgUntilMs = 0;

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

/* 信号条: 4 根递增柱状 (2/4/6/8px), level 0..4, 每格间隔 1px */
static void drawBars(int16_t x, int16_t y, uint8_t level, uint16_t color)
{
    if (level > 4) level = 4;
    for (uint8_t i = 0; i < 4; i++)
    {
        uint8_t h = 2 + i * 2;
        if (i < level)
            display.fillRect(x + i * 3, y + 8 - h, 2, h, color);
    }
}

/* WiFi 弧线图标 (🜿): 2 条同心圆弧(开口朝上 ±47°) + 底部中心圆点.
 * arcCount 控制画到第几条弧 (0=只圆点, 1=圆点+内弧, 2=全画), 用于重连/启动的递增循环动画.
 * scale 控制整体等比大小: scale=1 是主界面规格(厚2/间隙2/rBase=4/圆点半径1).
 *   scale=N 时: 弧厚=2N, 圆点半径=N, 层距=4N, 起始rBase=3N+1.
 *   结果: 圆点→内弧空白 = 弧间空白 = 2N, 永远相等, 比例与 scale=1 完全一致.
 * 左右绝对对称 (roundf 正负舍入一致). */
static void drawWifiLogo(int16_t cx, int16_t cy, int16_t rBase, uint16_t color,
                         uint8_t arcCount = 2, uint8_t scale = 1)
{
    const float deg2rad = 3.14159265f / 180.0f;
    display.fillCircle(cx, cy, scale, color);           /* 底部中心圆点 (半径=scale, 随缩放等比大) */
    const int THICK   = 2 * scale;                      /* 每条弧厚度: 2*N */
    const int LAYER_D = 4 * scale;                      /* 相邻弧内半径差: 厚2N + 间隙2N = 4N */
    if (arcCount > 2) arcCount = 2;
    for (int which = 0; which < arcCount; which++)
    {
        int inR  = rBase + which * LAYER_D;             /* 本层内半径 */
        int outR = inR + THICK - 1;                     /* 本层外半径(含端点, 实THICK px厚) */
        for (int off = -47; off <= 47; off += 1)
        {
            float rad = off * deg2rad;
            for (int rr = inR; rr <= outR; rr++)
            {
                int16_t x = cx + (int16_t)roundf(rr * sinf(rad));
                int16_t y = cy - (int16_t)roundf(rr * cosf(rad));
                display.drawPixel(x, y, color);
            }
        }
    }
}

/* MQTT 16x8 云朵: 实心+空心两张位图
 * 实心来自 oled-bitmapper weather-icons.c; 空心由用户手绘
 * 16w x 8h, 底=y+7. 画在 y=4 → 底=11 (与PGW001和WiFi底对齐)
 * mode0: 没网→空心云; mode1: 已连→实心云; mode2: 重连中→闪烁 */
static const unsigned char cloudSolid[] = {
    0x03, 0xC0, 0x07, 0xE0, 0x0F, 0xF0, 0x7F, 0xFE,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFE
};
static const unsigned char cloudHollow[] = {
    0x03, 0xC0, 0x04, 0x20, 0x08, 0x10, 0x70, 0x0E,
    0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x7F, 0xFE
};
static void drawMqttLogo(int16_t x, int16_t y, uint8_t mode)
{
    const unsigned char *bmp = (mode == 0) ? cloudHollow : cloudSolid;
    if (mode == 2 && ((millis() / OLED_ANIM_MS) & 1))
        return;   /* MQTT重连中: 隔帧隐藏 */
    display.drawBitmap(x, y, bmp, 16, 8, SSD1306_WHITE);
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

/* 节点"信号"等级 0..4: 基于节点上报帧携带的真实 RSSI(dBm) 换算.
 * 来源: LoRa 模块 DRSSI 附加字节 → lora_handler 存入 NodeData.rssi.
 * rssi=0 表示离线/未测到信号, 显示 0 格. (此前用距上次刷新时长模拟, 已废弃) */
static uint8_t sigLevel(const NodeData &nd)
{
    if (!nd.online || nd.rssi == 0) return 0;   /* 离线 或 未测到RSSI */
    if (nd.rssi > -55) return 4;                /* 信号很好 */
    if (nd.rssi > -67) return 3;                /* 好 */
    if (nd.rssi > -78) return 2;                /* 一般 */
    if (nd.rssi > -88) return 1;                /* 弱 */
    return 0;                                   /* 极弱/不可用 */
}

/* 当前确认存活的节点数 (online=true):
 * FOUND 显示的是"这次搜索真正确认存活(收到 PONG/数据/证书)"的节点,
 * 而不是 Flash 里有多少证书, 避免节点未插电仍显示已找到的假象 */
static uint8_t scanFound(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < nodeCount; i++)
        if (nodes[i].online) n++;
    return n;
}

/* ==================== Boot 动画绘制函数 ====================
 * 不引入 LVGL, 用 Adafruit_GFX 基础图元实现:
 *   - drawSpinner:    4 点围绕中心旋转, 8 帧/圈, 150ms/帧
 *   - drawProgressBar: 水平进度条 + 百分比文字 (预留扩展)
 *   - drawStatusIcon:   ✓ 或 ✗ 反白圆图标 (偶数帧亮, 实现闪烁)
 */

/* Spinner: 4 个点围绕中心旋转
 * frame: 0..7, 每帧整体旋转 45°, 视觉上像loader转圈 */
static void drawSpinner(int16_t cx, int16_t cy, uint8_t frame)
{
    const int16_t r = 7;  /* 旋转半径 */
    for (uint8_t i = 0; i < 4; i++)
    {
        int angle = (frame * 45 + i * 90) % 360;
        float rad = angle * 3.14159265f / 180.0f;
        int16_t x = cx + (int16_t)(r * cosf(rad));
        int16_t y = cy + (int16_t)(r * sinf(rad));
        display.fillCircle(x, y, 1, SSD1306_WHITE);
    }
}

/* ProgressBar: 水平进度条 + 百分比文字 (右对齐) */
static void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct)
{
    if (pct > 100) pct = 100;
    display.drawRect(x, y, w, h, SSD1306_WHITE);
    int16_t fillW = (w - 4) * pct / 100;
    if (fillW > 0)
        display.fillRect(x + 2, y + 2, fillW, h - 4, SSD1306_WHITE);
    /* 百分比文字紧跟右侧 */
    oled8x8Printf(x + w + 4, y, SSD1306_WHITE, "%d%%", pct);
}

/* StatusIcon: ✓ 或 ✗ 反白圆图标
 * frame 偶数=亮, 奇数=灭, 实现 150ms 闪烁
 * ok=true 画 ✓ (两段折线: 左下→中下→右上)
 * ok=false 画 ✗ (两条交叉线) */
static void drawStatusIcon(int16_t cx, int16_t cy, bool ok, uint8_t frame)
{
    if (frame % 2 != 0) return;  /* 奇数帧不画, 实现"灭" */
    /* 反白圆背景 */
    display.fillCircle(cx, cy, 9, SSD1306_WHITE);
    /* 黑色符号 */
    if (ok)
    {
        display.drawLine(cx - 4, cy,     cx - 1, cy + 3, SSD1306_BLACK);
        display.drawLine(cx - 1, cy + 3, cx + 5, cy - 4, SSD1306_BLACK);
    }
    else
    {
        display.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, SSD1306_BLACK);
        display.drawLine(cx - 4, cy + 4, cx + 4, cy - 4, SSD1306_BLACK);
    }
}

/* drawBootScreen: 根据 bootState 画对应画面
 * 布局 (128x64):
 *   y=0..15:  顶部标题 (居中, 8x8 字体)
 *   y=24..48: 中央动画区 (Spinner / StatusIcon)
 *   y=52..60: 底部辅助信息 (SSID/IP/原因, 居中) */
static void drawBootScreen(void)
{
    /* 帧计数: 150ms/帧 */
    uint32_t now = millis();
    if (now - bootLastFrameMs >= 150)
    {
        bootFrame++;
        bootLastFrameMs = now;
    }

    display.clearDisplay();

    /* 顶部标题 */
    const char *title = "";
    switch (bootState)
    {
        case BOOT_LOGO:          title = "GATEWAY V2";      break;
        case BOOT_WIFI_CONNECT:  title = "WIFI CONNECTING"; break;
        case BOOT_WIFI_OK:       title = "WIFI CONNECTED";  break;
        case BOOT_WIFI_FAIL:     title = "WIFI FAILED";     break;
        case BOOT_MQTT_CONNECT: title = "MQTT CONNECTING"; break;
        case BOOT_MQTT_OK:       title = "MQTT CONNECTED"; break;
        case BOOT_MQTT_FAIL:     title = "MQTT FAILED";    break;
        default: break;
    }
    if (title[0])
    {
        int16_t titleW = (int16_t)strlen(title) * 8;
        oled8x8Print((128 - titleW) / 2, 4, title, SSD1306_WHITE);
    }

    /* 中央动画区 */
    int16_t cx = 64, cy = 36;

    switch (bootState)
    {
        case BOOT_LOGO:
            /* Logo 静态: 居中显示项目名 */
            {
                const char *logo = "SMART-PARK";
                int16_t w = (int16_t)strlen(logo) * 8;
                oled8x8Print((128 - w) / 2, 36, logo, SSD1306_WHITE);
            }
            break;
        case BOOT_WIFI_CONNECT:
        {
            /* WiFi 递增加载动画, 节奏与 header 重连一致: 圆点→内弧→全画→停留, 150ms/帧循环.
             * scale=2 等比大一倍: 弧厚4/间隙4/圆点半径2, 三条间隙全部相等(都是4px),
             * 视觉比例和主界面(scale=1)完全一致, 只是撑满启动屏动画区更醒目. */
            uint8_t arcN = (uint8_t)(bootFrame % 4);
            arcN = (arcN == 3) ? 2 : arcN;
            drawWifiLogo(cx, cy, 7, SSD1306_WHITE, arcN, 2);
        }
        break;
        case BOOT_MQTT_CONNECT:
            drawSpinner(cx, cy, bootFrame);
            break;
        case BOOT_WIFI_OK:
        case BOOT_MQTT_OK:
            drawStatusIcon(cx, cy, true, bootFrame);
            break;
        case BOOT_WIFI_FAIL:
        case BOOT_MQTT_FAIL:
            drawStatusIcon(cx, cy, false, bootFrame);
            break;
        default:
            break;
    }

    /* 底部辅助信息 */
    if (bootDetail[0])
    {
        int16_t w = (int16_t)strlen(bootDetail) * 8;
        if (w > 128) w = 128;  /* 超屏宽: 左对齐截断 */
        oled8x8Print((128 - w) / 2, 52, bootDetail, SSD1306_WHITE);
    }

    display.display();
}

/* oled_setBootState: 切换 boot 状态 + 立即刷一帧
 * 不走 oled_refresh 间隔检查, 保证 setup 阶段即时反馈.
 * OLED 未就绪时安全降级 (只更新内部状态, 不画屏) */
void oled_setBootState(BootState state, const char *detail)
{
    bootState       = state;
    bootStateMs     = millis();
    bootFrame       = 0;  /* 进入新状态, 帧序号归零 */
    bootLastFrameMs = millis();

    if (detail)
    {
        strncpy(bootDetail, detail, sizeof(bootDetail) - 1);
        bootDetail[sizeof(bootDetail) - 1] = 0;
    }
    else
    {
        bootDetail[0] = 0;
    }

    /* BOOT_DONE: 标记完成, 让 startupPhase/scanPhase 归零,
     * 后续 oled_refresh 自动走 drawRuntime 主画面 */
    if (state == BOOT_DONE)
    {
        startupPhase = 0;
        scanPhase    = false;
    }

    /* 立即刷一帧 (BOOT_NONE/BOOT_DONE 不需要立即画, 后续 refresh 走主画面) */
    if (displayReady && state != BOOT_DONE && state != BOOT_NONE)
    {
        drawBootScreen();
        lastRefresh = millis();
    }
}

void oled_setBootProgress(uint8_t pct)
{
    bootProgress = pct > 100 ? 100 : pct;
}

/* 启动搜索节点画面: 全屏居中, 动画圆点 + 已发现节点数 + 提示 */
static void drawScanScreen(void)
{
    display.clearDisplay();

    /* 标题 + 动画点 (0.4s 轮换): SEARCHING NODES / . / .. / ... */
    static const char dot[4][4] = { "", ".", "..", "..." };
    uint8_t d = (millis() / 400) % 4;
    char title[24];
    snprintf(title, sizeof(title), "SEARCHING NODES%s", dot[d]);
    oled8x8Print((128 - (int16_t)strlen(title) * 8) / 2, 24, title, SSD1306_WHITE);

    /* 已确认存活节点数, 居中显示 (不显示扫描范围分母) */
    char found[16];
    snprintf(found, sizeof(found), "FOUND: %u", scanFound());
    oled8x8Print((128 - (int16_t)strlen(found) * 8) / 2, 40, found, SSD1306_WHITE);

    display.display();
}

/* startup screen: 单行垂直居中显示连接阶段 (无 STEP 序号)
 * phase 1=wifi 2=server(mqtt); phase 3 uses drawScanScreen() */
static void drawStartupScreen(uint8_t phase)
{
    display.clearDisplay();

    static const char *desc[2] = { "WIFI CONNECTING", "SERVER CONNECTING" };
    const char *d = (phase >= 1 && phase <= 2) ? desc[phase - 1] : "";
    oled8x8Print((128 - (int16_t)strlen(d) * 8) / 2, 28, d, SSD1306_WHITE);

    display.display();
}

/* switch startup phase (setup calls in order): 1=wifi 2=mqtt 3=scan */
void oled_showStartupPhase(uint8_t phase)
{
    startupPhase = phase;
    if (phase == 3)
    {
        oled_scanStart();
        /* 立即画出第一帧扫描画面: 否则进入主循环后可能因刷新间隔
         * 被跳过, 出现"SERVER CONNECTING 直接跳主界面"看不到第三屏 */
        drawScanScreen();
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
        DBG_PRINTLN("[OLED] 初始化失败 (未检测到屏幕)");
        return;
    }
    displayReady = true;

    /* 默认显示阶段1(网络连接中), 实际阶段由 setup 调 oled_showStartupPhase 切换 */
    scanPhase = true;
    startupPhase = 1;
    drawStartupScreen(1);
    DBG_PRINTLN("[OLED] 屏幕初始化成功");
}

/* setup 末尾调用: 扫描画面计时从主循环开始, 避免 setup 里
 * WiFi 阻塞等待(最长15s)把 8s/20s 的扫描时长耗尽 */
void oled_scanStart(void)
{
    scanStartMs = millis();
    scanPhase   = true;
}

/* 手动搜索 (短按 FLASH): 显示搜索节点动画.
 * 与开机扫描共用同一套退出条件 (扫描结束 + 展示满最短时长) */
void oled_startManualScan(void)
{
    scanStartMs  = millis();
    scanPhase    = true;
    startupPhase = 0;   /* 离开启动屏状态, 结束后回运行主界面 */
}

/* 配网模式显示: WiFi图标 + AP名/密码 + 操作指引 (全英文, 无中文字库依赖)
 * 顶部: WiFi图标 + "AP CONFIG" 标题
 * 中部: Connect(热点名) / Pass(密码)
 * 底部: 分隔线 + "Open any website" 指引 + IP 反显块(醒目) */
static void drawConfigPortal(void)
{
    display.clearDisplay();

    /* 顶部: WiFi 图标 + 标题 (图标直径14px, 标题右移避让) */
    drawWifiLogo(14, 9, 4, SSD1306_WHITE);
    oled8x8Print(26, 4, "AP CONFIG", SSD1306_WHITE);

    /* 连接信息: SSID + 密码 (各占一行) */
    oled8x8Printf(0, 16, SSD1306_WHITE, "SSID:%s", CONFIG_AP_SSID);
    oled8x8Printf(0, 28, SSD1306_WHITE, "Pass:%s", CONFIG_AP_PASSWORD);

    /* 分隔线 */
    display.drawLine(0, 39, 127, 39, SSD1306_WHITE);

    /* 操作指引 */
    oled8x8Print(0, 42, "Open to config:", SSD1306_WHITE);

    /* IP 反显块 (醒目, 128x64 底部) */
    display.fillRect(4, 53, 120, 10, SSD1306_WHITE);
    oled8x8Print(24, 54, "192.168.4.1", SSD1306_BLACK);

    display.display();
}

/* Header: 设备名+WiFi信号条 / MQTT点状态(右对齐)
 * 屏幕为双色屏: 上 1/4(0..15px) 黄色, 下 3/4(16..63px) 蓝色.
 * Header 独占黄色区 (y=4 垂直居中), 节点行/Footer 全在蓝色区
 * 节点在线状态由下方节点行体现, Header 不再重复显示 LoRa 点 */
static void drawHeader(void)
{
    oled8x8Print(0, 4, ONENET_DEVID, SSD1306_WHITE);          /* 设备名 PGW001 */

    /* WiFi 图标 (🜿): 已连=两条弧全画; 重连中=信号格式递增(只圆点→圆点+内弧→全画, 循环).
     * 右 WiFi (cx=100), 右侧 MQTT 云朵 (x=112, 16px 宽), 左 PGW001 文字, 三者之间均留空白 */
    if (WiFi.status() == WL_CONNECTED)
        drawWifiLogo(100, 10, 4, SSD1306_WHITE, 2);
    else
    {
        uint8_t arcN = (uint8_t)((millis() / OLED_ANIM_MS) % 4);   /* 0/1/2/3 */
        if (arcN == 3) arcN = 0;
        /* 节奏: 帧0(圆点) → 帧1(圆点+内弧) → 帧2(全画) → 帧3(再全画一拍, 视觉停留) */
        arcN = (arcN == 3) ? 2 : arcN;
        drawWifiLogo(100, 10, 4, SSD1306_WHITE, arcN);
    }

    /* MQTT 状态图标(16x8 云朵, 最右贴边):
     * 已连=实心云; WiFi在线但MQTT重连中=云闪烁; 全断=空心云 */
    if (sysEventFlag & SYS_EVENT_MQTT_CONNECTED)
        drawMqttLogo(112, 4, 1);
    else if (WiFi.status() == WL_CONNECTED)
        drawMqttLogo(112, 4, 2);
    else
        drawMqttLogo(112, 4, 0);
}

/* Footer: 上/下每分钟消息速率 + 页码
 * 速率 = 当前刷新间隔内的新增消息数 / 实际流逝秒数 × 60 (滑动平均)
 * 固定 2 位 (超 99 显示 99), 保证左右对齐 */
static void drawFooter(uint8_t page, uint8_t pages)
{
    static uint32_t lastRateMs  = 0;
    static uint32_t lastTxTotal = 0;
    static uint32_t lastRxTotal = 0;
    static uint32_t txPerMin    = 0;
    static uint32_t rxPerMin    = 0;

    uint32_t now = millis();
    uint32_t dt  = now - lastRateMs;
    if (dt >= 1000)               /* 每秒(或更长间隔)更新一次速率 */
    {
        txPerMin = (mqttTxCount - lastTxTotal) * 60000UL / dt;
        rxPerMin = (mqttRxCount - lastRxTotal) * 60000UL / dt;
        lastTxTotal = mqttTxCount;
        lastRxTotal = mqttRxCount;
        lastRateMs  = now;
    }

    uint32_t tx = (txPerMin > 99) ? 99 : txPerMin;
    uint32_t rx = (rxPerMin > 99) ? 99 : rxPerMin;

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

/* 显示一行临时消息, 持续 durationMs 毫秒后自动恢复正常画面.
 * 非阻塞: 仅记下消息和到期时间, 实际绘制由 oled_refresh() 周期完成.
 * OLED 未就绪时本函数也安全调用 (内部不直接写屏, 仅设状态) */
void oled_showTempMessage(const char *msg, uint32_t durationMs)
{
    if (msg == 0 || msg[0] == 0 || durationMs == 0)
    {
        /* 空消息或零时长: 视为取消当前临时消息 */
        tempMsgBuf[0]  = 0;
        tempMsgUntilMs = 0;
        return;
    }
    /* 截断到缓冲区容量-1 (最多 23 字符, 屏宽 128/8=16 实际显示 16) */
    strncpy(tempMsgBuf, msg, sizeof(tempMsgBuf) - 1);
    tempMsgBuf[sizeof(tempMsgBuf) - 1] = 0;
    tempMsgUntilMs = millis() + durationMs;   /* 到期时间戳 */
}

void oled_refresh(void)
{
    if (!displayReady) return;

    uint32_t now = millis();

    /* 临时消息(调试/告警): 优先级最高, 在有效期内全屏居中显示,
     * 到期自动清空, 恢复正常画面 */
    if (tempMsgUntilMs != 0 && tempMsgBuf[0] != 0)
    {
        /* 用有符号差值判到期, 防 millis 回绕 */
        int32_t remaining = (int32_t)(tempMsgUntilMs - now);
        if (remaining > 0)
        {
            display.clearDisplay();
            /* 屏宽 128, 8x8 字体 -> 一行最多 16 字符; 居中起始 x */
            int16_t w = (int16_t)strlen(tempMsgBuf) * 8;
            if (w > 128) w = 128;
            oled8x8Print((128 - w) / 2, 28, tempMsgBuf, SSD1306_WHITE);
            display.display();
            /* 临时消息期间用快档刷新, 保证到期立即切换 */
            lastRefresh = now;
            return;
        }
        /* 到期: 清空消息槽, 下面的流程正常走 */
        tempMsgBuf[0]   = 0;
        tempMsgUntilMs  = 0;
    }

    /* Boot 状态机画面 (setup 启动阶段, 新逻辑):
     * 覆盖旧 startupPhase 1/2 的静态画面, 提供 spinner 旋转动画 +
     * 成功/失败视觉反馈. 节点扫描(原 phase 3)仍走下面 scanPhase 旧逻辑.
     * boot 期间用 100ms 快档刷新, 保证 spinner/blink 动画流畅 */
    if (bootState != BOOT_NONE && bootState != BOOT_DONE)
    {
        if (now - lastRefresh < 100) return;
        lastRefresh = now;
        drawBootScreen();
        return;
    }

    /* 刷新间隔: 扫描画面(第三屏)或重连动画时用快档 0.45s,
     * 保证 FOUND 数字/动画点实时更新; 全部在线才恢复 2s 慢刷.
     * 注意: 扫描画面若按 2s 慢刷, 会因搜索 1.6s 就结束而只画到
     * 第一帧(0/2), 数字没机会更新就被退出逻辑跳走 */
    uint32_t interval = OLED_REFRESH_MS;
    if (scanPhase ||
        WiFi.status() != WL_CONNECTED ||
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

    /* 扫描节点画面: 全屏居中显示, 满足条件后自动切换到主界面:
     *  a) 扫描真实结束 (discoveryMode 清除, 扫完一轮)
     *  b) 且已展示满 OLED_SCAN_MIN_MS (最短展示时长, 保证用户看清第三屏)
     *  c) OLED_SCAN_MAX_MS 兜底退出 (保险, 正常情况下扫描一轮必会结束) */
    if (scanPhase)
    {
        uint32_t elapsed = now - scanStartMs;
        bool done = !lora_discoveryActive() && (elapsed >= OLED_SCAN_MIN_MS);
        if (done || elapsed >= OLED_SCAN_MAX_MS)
        {
            scanPhase    = false;
            startupPhase = 0;   /* scan done, enter main UI */
        }
        else
        {
            drawScanScreen();
            return;
        }
    }

    /* 运行模式主界面: WiFi 掉线时也照常显示 (Header 信号条走动画、MQTT 点空心),
     * WiFi 重连过程状态输出到串口日志, 不占屏幕 */
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
