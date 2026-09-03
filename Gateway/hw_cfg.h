/* hw_cfg.h - 板级硬件配置 (引脚/串口/地址)
 *
 * 换开发板时才需要修改本文件:
 *   - LoRa 模块串口 (ESP32 硬串口 / ESP8266 软串)
 *   - OLED I2C 引脚与地址
 *   - 配网长按按键引脚
 *   - 调试串口选择
 */
#ifndef HW_CFG_H
#define HW_CFG_H

/* ==================== LoRa 串口配置 ====================
 * - ESP8266(只有1个硬串口): 使用SoftwareSerial
 *     LORA_BAUD 软串建议 9600, 115200不稳
 * - ESP32/S3(3个硬串口): 直接 UART1, 引脚自由配置 */
#if defined(ESP32)
  #define LORA_USE_HWSERIAL    1
  #define LORA_RX_PIN          4
  #define LORA_TX_PIN          5
  #define LORA_BAUD            115200
#else
  #define LORA_USE_HWSERIAL    0
  #define LORA_RX_PIN          D1
  #define LORA_TX_PIN          D0
  #define LORA_BAUD            9600
#endif

/* ==================== LoRa 模块 AUX 引脚 ====================
 * AUX 是模块输出, 反映模块忙闲状态:
 *   高=数据发送中/接收中/模式切换中(忙)
 *   低=发送完成/接收完成/切换完成(闲)
 * 接 D2 (GPIO4), 发完命令后 while 等 AUX 变高, 判断模块是否收到数据 */
#if defined(ESP32)
  #define LORA_AUX_PIN          27
#else
  #define LORA_AUX_PIN          D2    /* GPIO4 */
#endif
#define LORA_AUX_WAIT_MS       50UL   /* 等 AUX 变高的超时(ms) */

/* ==================== OLED 显示配置 (I2C, 集中显示) ====================
 * 0.96寸 SSD1306 接到网关, 显示 WiFi/MQTT/各节点状态
 * ESP8266: SDA=D6(GPIO12), SCL=D7(GPIO13) (不与 LoRa 软串 D0/D1 / AUX D2 / 按键 D3 冲突) */
#if defined(ESP32)
  #define OLED_SDA_PIN      18
  #define OLED_SCL_PIN      19
#else
  #define OLED_SDA_PIN      D6
  #define OLED_SCL_PIN      D7
#endif
#define OLED_I2C_ADDR       0x3C    /* SSD1306 I2C 地址 */

/* ==================== 配网长按按键引脚 ==================== */
#if defined(ESP32)
  #define CONFIG_KEY_PIN    0    /* ESP32 开发板 BOOT 按键 (GPIO0) */
#else
  #define CONFIG_KEY_PIN    0    /* NodeMCU 板载 FLASH 按键 (D3/GPIO0) */
#endif

/* ==================== 调试串口 ==================== */
#if defined(ESP32)
  #define DEBUG_SERIAL        Serial
#elif LORA_USE_ESP8266_HWSERIAL
  /* 模式2: 调试输出走 UART1 TX (GPIO2/D4), 需额外 USB-TTL 转接 */
  #define DEBUG_SERIAL        Serial1
#else
  /* 模式1: 调试输出走 USB 串口 (Serial), Arduino IDE 串口监视器直接看 */
  #define DEBUG_SERIAL        Serial
#endif
#define DEBUG_BAUD          115200

#endif /* HW_CFG_H */
