#include "esp_camera.h"
#include "img_converters.h"

// ==================== 配置区 ====================

// 工作模式: 1=串口转发模式(拍照->串口发JPEG->电脑跑OCR), 0=WiFi直发百度OCR模式(旧)
#define SERIAL_BRIDGE_MODE 1

#if !SERIAL_BRIDGE_MODE
// === 旧模式: WiFi直发百度 OCR 才需要的配置 ===
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// Token 模式: 1=硬编码(推荐), 0=动态获取
#define USE_HARDCODED_TOKEN 1

// WiFi 信息
const char* ssid = "Aira";
const char* passwd = "20231111";

// 百度云 API 凭证
const char* apiKey = "7dTZbfdpp2iLH1YwP9i14YXy";
const char* secretKey = "OPQjMV0AMhtEQP5Xek2qr16WQ9jeSV50";

// 硬编码 Token (有效期 30 天, 2026-09-13 过期)
const char* HARDCODED_TOKEN = "24.dad2e22d9f8fd83f3935dc460bdff3f7.2592000.1789306747.282335-124141210";

// 百度 OCR 地址
const char* OCR_HOST = "aip.baidubce.com";
const char* OCR_PATH = "/rest/2.0/ocr/v1/license_plate";

// 重试配置
#define OCR_RETRY_MAX 2       // OCR 请求最多重试次数
#define TLS_TIMEOUT_MS 30000  // TLS 握手超时 (毫秒)
#endif

// === 串口帧协议 (SERIAL_BRIDGE_MODE=1 时生效) ===
// 帧格式: "$IMG,<len>\n" + <JPEG 二进制数据 len 字节> + <CRC32 4 字节大端> + "\n$END\n"
// 接收端(Python)按行读 header, 解析 len, 然后精确读取 len+4 字节并校验 CRC32

// ==================== 摄像头引脚 (ESP32-S3-EYE) ====================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

#define BOOT_BTN_PIN 0  // GPIO0 = 板载 BOOT 按钮

// ==================== 全局变量 ====================
bool lastBtnState = HIGH;
bool recognizeInProgress = false;
#if !SERIAL_BRIDGE_MODE
DynamicJsonDocument document(8192);
const char base64Map[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
#endif

// ==================== 函数声明 ====================
void vCameraInit(void);
void recognizePlate();
#if SERIAL_BRIDGE_MODE
// 串口转发模式: 把 JPEG 通过帧协议发到电脑
void sendImageToSerial(const uint8_t *jpgBuf, size_t jpgLen);
#else
char* base64_encode(char *buf, int len);
// 内部实现: 手动TLS握手 + 2KB分块写body (ESP32 mbedTLS缓存仅16KB, 大body必须分块)
int httpTlsPost(const char* host, const char* path, const char* body, size_t bodyLen, String& response);
#endif

void setup() {
  Serial.begin(115200);
  Serial.println("=== 车牌识别系统启动 ===");

  // BOOT 按钮引脚 (内部上拉, 按下为 LOW)
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  vCameraInit();

#if SERIAL_BRIDGE_MODE
  // 串口转发模式: 无需 WiFi, 直接待命
  Serial.println("======================================");
  Serial.println("[模式] 串口转发: 拍照->串口发JPEG->电脑跑OCR");
  Serial.println("系统就绪! 按下BOOT按钮(GPIO0)拍照并传输");
  Serial.println("======================================");
#else
  // WiFi直发模式: 连接 WiFi 用于直连百度 OCR
  WiFi.mode(WIFI_STA);                                   // 显式设置为STA模式
  Serial.print("正在连接WiFi");
  WiFi.begin(ssid, passwd);
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(1000);
    Serial.print(".");
    wifiTimeout++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi已连接, IP: %s\n", WiFi.localIP().toString().c_str());
    WiFi.setSleep(false);                                  // 禁用省电, 避免发送大body时WiFi休眠导致超时
    WiFi.setTxPower(WIFI_POWER_19_5dBm);                   // 最大发射功率, 减少重传
  } else {
    Serial.printf("\nWiFi连接失败, status=%d\n", WiFi.status());
    return;
  }

  Serial.println("======================================");
  Serial.println("[模式] WiFi直发: 拍照->WiFi发百度OCR");
  Serial.println("系统就绪! 按下BOOT按钮(GPIO0)开始拍照识别");
  Serial.println("======================================");
#endif
}

void loop() {
  if (recognizeInProgress) return;  // 正在拍照识别中, 忽略按键

  bool curState = digitalRead(BOOT_BTN_PIN);

  // 下降沿触发: 上次是HIGH(没按), 这次是LOW(按下)
  if (lastBtnState == HIGH && curState == LOW) {
    // 去抖: 等50ms再确认
    delay(50);
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
      Serial.println("\n>>> BOOT按钮按下, 开始拍照识别...");
      recognizeInProgress = true;
      recognizePlate();
      Serial.println(">>> 识别完成, 再次按BOOT按钮可重新拍照");
      recognizeInProgress = false;
      // 等用户松开按钮, 避免长按重复触发
      while (digitalRead(BOOT_BTN_PIN) == LOW) delay(10);
    }
  }
  lastBtnState = curState;
  delay(10);  // 主循环小延时, 降低CPU占用
}



// 拍照 + 传输/识别车牌
#if SERIAL_BRIDGE_MODE
// === 串口转发模式: 拍照后通过帧协议发到电脑, 由电脑调用百度OCR ===
void recognizePlate() {
  // 丢弃前10帧(约1秒), 让传感器自动曝光/白平衡完全收敛
  for (int i = 0; i < 10; i++) {
    camera_fb_t *tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(100);
  }

  // 正式拍照
  camera_fb_t * fb = esp_camera_fb_get();
  if (fb == NULL) {
    Serial.println("[错误] 拍照失败");
    return;
  }
  Serial.printf("[拍照] %dx%d 格式=%d 大小=%d字节\n",
                fb->width, fb->height, fb->format, fb->len);

  // JPEG 编码 (质量60: 平衡体积与识别率)
  uint8_t *jpgBuf = NULL;
  size_t jpgLen = 0;
  bool jpeg_ok = frame2jpg(fb, 60, &jpgBuf, &jpgLen);
  esp_camera_fb_return(fb);
  fb = NULL;

  if (!jpeg_ok || jpgBuf == NULL) {
    Serial.println("[错误] JPEG编码失败");
    return;
  }
  Serial.printf("[JPEG] %d 字节\n", (int)jpgLen);

  // 通过帧协议发送到串口
  sendImageToSerial(jpgBuf, jpgLen);

  free(jpgBuf);
  Serial.println("[完成] 图片已通过串口发送, 等待电脑 OCR");
}
#else
// === WiFi直发模式: 拍照后直接调用百度OCR ===
void recognizePlate()

{

  // 丢弃前10帧(约1秒), 让传感器自动曝光/白平衡完全收敛

  for (int i = 0; i < 10; i++) {

    camera_fb_t *tmp = esp_camera_fb_get();

    if (tmp) esp_camera_fb_return(tmp);

    delay(100);

  }



  // 正式拍照

  camera_fb_t * fb = esp_camera_fb_get();

  if (fb == NULL) {

    Serial.println("拍照失败");

    return;

  }



  Serial.printf("拍照完成: %dx%d 格式=%d 大小=%d字节\n",

                fb->width, fb->height, fb->format, fb->len);



  // 直接用原始摄像头帧缓冲做 JPEG 编码 (摄像头安装方向正确, 无需旋转)
  uint8_t *jpgBuf = NULL;
  size_t jpgLen = 0;
  bool jpeg_ok = frame2jpg(fb, 60, &jpgBuf, &jpgLen);  // 质量60: 实测识别率≈80, 但JPEG体积约小50%, 发送时间减半
  esp_camera_fb_return(fb);
  fb = NULL;



  if (!jpeg_ok || jpgBuf == NULL) {

    Serial.println("JPEG编码失败");

    return;

  }



  Serial.printf("JPEG编码完成: %d字节\n", jpgLen);



  // Base64编码

  char *b64 = base64_encode((char*)jpgBuf, jpgLen);

  free(jpgBuf);



  if (b64 == NULL) {

    Serial.println("Base64编码失败");

    return;

  }



  Serial.printf("Base64编码完成: %d字符\n", strlen(b64));
  // === Base64 串口输出 ===
#if PRINT_BASE64_TO_SERIAL
  Serial.println("===BEGIN BASE64===");
  {
    size_t b64Len = strlen(b64);
    size_t i = 0;
    while (i < b64Len) {
      size_t n = (b64Len - i >= 64) ? 64 : (b64Len - i);
      char tmp[65];
      memcpy(tmp, b64 + i, n);
      tmp[n] = '\0';
      Serial.println(tmp);
      i += n;
    }
  }
  Serial.println("===END BASE64===");
#endif



  // 获取百度access_token
  String token = "";
#if USE_HARDCODED_TOKEN
  // 模式1: 直接用硬编码的Token (省一次HTTP请求, 避免WiFi不稳时失败)
  token = String(HARDCODED_TOKEN);
  Serial.printf("[Token] 使用硬编码Token, 长度=%d\n", token.length());
#else
  // 模式2: 动态请求Token (Token过期后用这个模式获取一次新的)
  Serial.println("[Token] 动态获取中...");
  token = getAccessToken();
#endif
  if (token.length() == 0) {
    Serial.println("获取Token失败, 无法识别");
    free(b64);
    return;
  }



  // === 构建 HTTP body (C风格, 避免String类对大数据的内存问题) ===
  // 参考 百度官方C++示例: image=<URL编码的Base64>
  // Base64字符集中只有 '+', '/', '=' 需要URL编码 (其他字母数字不编码)
  size_t b64Len = strlen(b64);
  size_t encodedLen = 0;
  for (size_t i = 0; i < b64Len; i++) {
    unsigned char c = (unsigned char)b64[i];
    if (c == '+' || c == '/' || c == '=' ||
        c == '?' || c == '#' || c == '&') {
      encodedLen += 3;  // %XX 占3字节
    } else {
      encodedLen += 1;
    }
  }
  size_t bodyLen = 6 + encodedLen + 1;  // "image=" + encoded + '\0'
  char *body = (char*)malloc(bodyLen);
  if (body == NULL) {
    Serial.println("Body内存分配失败");
    free(b64);
    return;
  }
  strcpy(body, "image=");
  char *p = body + 6;
  const char *hex = "0123456789ABCDEF";  // 大写十六进制, 与官方示例一致
  for (size_t i = 0; i < b64Len; i++) {
    unsigned char c = (unsigned char)b64[i];
    if (c == '+' || c == '/' || c == '=' ||
        c == '?' || c == '#' || c == '&') {
      *p++ = '%';
      *p++ = hex[(c >> 4) & 0x0F];
      *p++ = hex[c & 0x0F];
    } else {
      *p++ = (char)c;
    }
  }
  *p = '\0';
  free(b64);  // body构建完成, 释放原始base64

  Serial.printf("Body构建完成, 长度=%d bytes\n", (int)strlen(body));



  // === 发送OCR请求 (含重试) ===
  size_t bodySize = strlen(body);
  Serial.printf("Body长度: %d bytes\n", (int)bodySize);

  int httpCode = -1;
  String respBody = "";

  for (int retry = 0; retry <= OCR_RETRY_MAX; retry++) {
    if (retry > 0) {
      Serial.printf("\n[重试 %d/%d] 等待1秒...\n", retry, OCR_RETRY_MAX);
      delay(1000);
    }

    // WiFi状态检查与重连
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[WiFi] 已断开(status=%d), 尝试重连...\n", WiFi.status());
      WiFi.reconnect();
      int wfRetry = 0;
      while (WiFi.status() != WL_CONNECTED && wfRetry < 20) {
        delay(500); Serial.print("."); wfRetry++;
      }
      Serial.println();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WiFi] 重连失败, 跳过本次重试\n");
        continue;
      }
      Serial.printf("[WiFi] 重连成功, IP=%s, RSSI=%d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }

    // 构建完整path (含access_token参数)
    String fullPath = String(OCR_PATH) + "?access_token=" + token;

    // 一行发送: 内部自动TLS握手+分块body+解析响应码, 主流程和官方curl示例一样干净
    Serial.printf("[%d/%d] POST %d bytes...\n", retry+1, OCR_RETRY_MAX+1, (int)bodySize);
    unsigned long reqStart = millis();
    httpCode = httpTlsPost(OCR_HOST, fullPath.c_str(), body, bodySize, respBody);

    Serial.printf("[%d/%d] HTTP %d, 耗时%lums, 响应%d字节\n",
                  retry+1, OCR_RETRY_MAX+1, httpCode, millis()-reqStart, (int)respBody.length());
    if (httpCode == 200) break;
  }

  free(body);

  // 打印完整原始响应体
  Serial.println("┌───────────────────────────────────────┐");
  Serial.println("│  百度API原始响应体 (JSON)              │");
  Serial.println("└───────────────────────────────────────┘");
  Serial.println(respBody);
  Serial.println("─────────────────────────────────────────");

  if (httpCode != 200) {
    Serial.println("===== OCR 请求失败 (已重试全部次数) =====");
    Serial.printf("HTTP返回码: %d\n", httpCode);
    Serial.printf("WiFi状态  : %d (3=已连接, 6=已断开)\n", WiFi.status());
    Serial.printf("信号强度  : %d dBm\n", WiFi.RSSI());
    Serial.println("=========================================");
    return;
  }



  // 解析JSON结果

  DeserializationError error = deserializeJson(document, respBody);

  if (error) {

    Serial.print("JSON解析失败: ");

    Serial.println(error.c_str());

    Serial.println(respBody);

    return;

  }



  // 提取车牌信息
  // 按百度API实际返回的JSON结构直接解析:
  // {"words_result":{"number":"京Q06666","color":"blue","probability":[...],"vertexes_location":[...]},"log_id":...}
  JsonObject words = document["words_result"].as<JsonObject>();
  if (!words) {
    Serial.println("错误: words_result 字段为空或不存在");
    Serial.println(respBody);
    return;
  }

  String plate = words["number"];                              // 车牌号码
  String color = words["color"];                                // 车牌颜色
  uint64_t logId = document["log_id"];                          // 日志ID

  // probability 是7个浮点数的数组, 计算平均置信度
  float avgProb = 0.0f;
  JsonArray probs = words["probability"].as<JsonArray>();
  if (probs.size() > 0) {
    float sum = 0;
    for (float p : probs) sum += p;
    avgProb = sum / probs.size();
  }

  // vertexes_location 是4个顶点坐标的数组 (左上/右上/右下/左下)
  JsonArray vertexes = words["vertexes_location"].as<JsonArray>();

  Serial.println("========================================");
  Serial.printf("车牌号码: %s\n", plate.c_str());
  Serial.printf("车牌颜色: %s\n", color.c_str());
  Serial.printf("置信度:   %.2f%%\n", avgProb * 100);
  Serial.printf("Log ID:   %llu\n", (unsigned long long)logId);
  if (vertexes.size() >= 4) {
    Serial.printf("车牌位置: (%d,%d) -> (%d,%d)\n",
                  vertexes[0]["x"].as<int>(), vertexes[0]["y"].as<int>(),
                  vertexes[2]["x"].as<int>(), vertexes[2]["y"].as<int>());
  }
  Serial.println("========================================");

}
#endif



#if SERIAL_BRIDGE_MODE
// ========== CRC32 查表法 (IEEE 802.3 多项式 0xEDB88320, 与 zlib/PNG 一致) ==========
static uint32_t crc32_table[256];
static bool crc32_table_inited = false;
static void crc32_init() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
    }
    crc32_table[i] = c;
  }
  crc32_table_inited = true;
}
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
  if (!crc32_table_inited) crc32_init();
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

// ========== 串口帧协议发送 ==========
// 帧: "$IMG,<len>\n" + <JPEG 二进制 len 字节> + <CRC32 4字节大端> + "\n$END\n"
// 发送策略: 先发帧头(让电脑切换到接收二进制状态), 然后分块 write,
//          每块 4KB, 与 Serial 默认 TX 缓冲配合, 调用 yield() 让 USB CDC 持续吐数据
void sendImageToSerial(const uint8_t *jpgBuf, size_t jpgLen) {
  // 1. 计算 CRC32
  uint32_t crc = crc32_compute(jpgBuf, jpgLen);

  // 2. 发送帧头: "$IMG,<len>\n"  (len 用十进制, 方便人工读串口日志)
  Serial.printf("$IMG,%u\n", (unsigned)jpgLen);

  // 3. 分块发送 JPEG 二进制 (每块 4KB, 避免 Serial 默认缓冲堵住)
  const size_t CHUNK = 4096;
  size_t remaining = jpgLen;
  const uint8_t *p = jpgBuf;
  unsigned long tStart = millis();
  while (remaining > 0) {
    size_t chunk = (remaining > CHUNK) ? CHUNK : remaining;
    size_t written = Serial.write(p, chunk);
    if (written == 0) {
      // TX 缓冲满, 等一下再试
      delay(2);
      continue;
    }
    p += written;
    remaining -= written;
    yield();  // 让 USB CDC 任务继续传输
  }
  Serial.flush();  // 等待 TX FIFO 全部发完

  // 4. 发送 CRC32 (4字节大端, 二进制)
  uint8_t crcBytes[4] = {
    (uint8_t)(crc >> 24),
    (uint8_t)(crc >> 16),
    (uint8_t)(crc >> 8),
    (uint8_t)(crc & 0xFF)
  };
  Serial.write(crcBytes, 4);
  Serial.flush();

  // 5. 发送帧尾
  Serial.println("\n$END");

  Serial.printf("[TX] %u 字节 JPEG + CRC32=0x%08X, 耗时 %lums\n",
                (unsigned)jpgLen, crc, millis() - tStart);
}
#endif







#if !SERIAL_BRIDGE_MODE
char* base64_encode(char* buf,int len)

{

  int retLen = ceil(len*1.0/3*4);

  char *retBuf = (char*)malloc(sizeof(char)*(retLen+4));
  if (!retBuf) return NULL;

  int index=0;

  int currIndex=0;

  int i=0;

  int lastCnt = len%3;

  

  for(i=0;i<(len-lastCnt);i+=3){

    index = ( (buf[i] & 0xFC) >> 2);

    retBuf[currIndex]=base64Map[index];

    index = ( ( (buf[i] & 0x03) <<4) + ( (buf[i+1] & 0xF0) >> 4) );

    retBuf[currIndex+1]=base64Map[index];

    index = ( ( (buf[i+1] & 0x0F) << 2) + ( (buf[i+2] & 0xC0) >> 6) );

    retBuf[currIndex+2]=base64Map[index];

    index = (buf[i+2] & 0x3F);

    retBuf[currIndex+3]=base64Map[index];

    currIndex+=4;

  }

  

  if(lastCnt==1){

    index = ( (buf[i] & 0xFC) >> 2);

    retBuf[currIndex]=base64Map[index];

    index = ( (buf[i] & 0x03) << 4);

    retBuf[currIndex+1]=base64Map[index];

    retBuf[currIndex+2]='=';

    retBuf[currIndex+3]='=';

    currIndex+=4;

  }else if(lastCnt==2){

    index = ( (buf[i] & 0xFC) >> 2);

    retBuf[currIndex]=base64Map[index];

    index = ( ( (buf[i] & 0x03) <<4) + ( (buf[i+1] & 0xF0) >> 4) );

    retBuf[currIndex+1]=base64Map[index];

    index = ( (buf[i+1] & 0x0F) << 2);

    retBuf[currIndex+2]=base64Map[index];

    retBuf[currIndex+3]='=';

    currIndex+=4;

  }

  

  retBuf[currIndex]='\0';

  return retBuf;

}
#endif  // !SERIAL_BRIDGE_MODE



void vCameraInit(void)

{

  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;

  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;

  config.pin_d1 = Y3_GPIO_NUM;

  config.pin_d2 = Y4_GPIO_NUM;

  config.pin_d3 = Y5_GPIO_NUM;

  config.pin_d4 = Y6_GPIO_NUM;

  config.pin_d5 = Y7_GPIO_NUM;

  config.pin_d6 = Y8_GPIO_NUM;

  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;

  config.pin_pclk = PCLK_GPIO_NUM;

  config.pin_vsync = VSYNC_GPIO_NUM;

  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;

  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;

  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  config.pixel_format = PIXFORMAT_RGB565;

  config.frame_size = FRAMESIZE_VGA;     // 640x480, 适合车牌识别

  config.fb_count = 2;

  config.fb_location = CAMERA_FB_IN_PSRAM;

  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;



  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {

    Serial.printf("摄像头初始化失败: 0x%x\r\n", err);

    return;

  }



  sensor_t * s = esp_camera_sensor_get();

  if (s == NULL) {

    Serial.println("摄像头传感器为空!");

    return;

  }



  Serial.printf("摄像头型号: 0x%x\r\n", s->id.PID);



  // === 图像质量调节 ===

  s->set_brightness(s, 0);    // 亮度默认

  s->set_contrast(s, 1);      // 对比度+1, 车牌字符更清晰

  s->set_saturation(s, 0);    // 饱和度默认

  // 保持自动曝光和自动白平衡开启, 让传感器自动调节

  s->set_exposure_ctrl(s, 1);

  s->set_whitebal(s, 1);

  // 最大增益限制到4倍, 减少噪点

  s->set_gainceiling(s, GAINCEILING_4X);



  // GC0308/GC032A 需要镜像/翻转处理

  if (s->id.PID == GC0308_PID) {

    s->set_hmirror(s, 0);

  } else if (s->id.PID == GC032A_PID) {

    s->set_vflip(s, 1);

  } else if (s->id.PID == OV2640_PID || s->id.PID == OV3660_PID) {

    s->set_vflip(s, 1);

  }

}


#if !SERIAL_BRIDGE_MODE
// ========== 内部传输: HTTPS POST (自动TLS握手 + 2KB分块写body) ==========
// ESP32的mbedTLS写缓冲只有16KB, 超过必须分块写. 此函数封装所有细节, 调用者只需一行.
// 返回值: HTTP状态码(如200), 负数=底层错误(-1连接超时, -2写入卡死, -3解析失败)
int httpTlsPost(const char* host, const char* path, const char* body, size_t bodyLen, String& response) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);

  // 1. TLS握手 (保持30s超时, 弱网下必须留够时间)
  unsigned long t0 = millis();
  if (!client.connect(host, 443, TLS_TIMEOUT_MS)) {
    client.stop();
    return -1;
  }
  Serial.printf("[TLS] 握手耗时 %lums\n", millis() - t0);

  // 2. 一次性组装并发送HTTP请求头 (避免7次小write各自触发TLS加密)
  String header;
  header.reserve(256);
  header += "POST ";
  header += path;
  header += " HTTP/1.1\r\nHost: ";
  header += host;
  header += "\r\nContent-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\nContent-Length: ";
  header += (int)bodyLen;
  header += "\r\nConnection: close\r\n\r\n";
  client.print(header);
  client.flush();

  // 3. 分块写body (8KB分块, 远小于mbedTLS 16KB record上限, 减少加密调用次数)
  //    历史教训: 16KB是ESP32 mbedTLS固定SSL缓冲, 分块必须<<16KB才能稳
  const uint8_t* bp = (const uint8_t*)body;
  size_t remaining = bodyLen;
  const size_t CHUNK = 8192;
  int failCount = 0;
  unsigned long bStart = millis();
  size_t sentPrev = 0;
  unsigned long tPrev = bStart;
  while (remaining > 0) {
    size_t chunk = (remaining > CHUNK) ? CHUNK : remaining;
    size_t written = client.write(bp, chunk);
    if (written == 0) {
      failCount++;
      if (failCount > 500) { client.stop(); return -2; }
      delay(2);  // TCP窗口满通常只等几毫秒, 20ms过于保守浪费时间
      continue;
    }
    failCount = 0;
    bp += written;
    remaining -= written;
    // 每10KB打印一次进度, 观察发送是否真的有进展
    if ((bodyLen - remaining - sentPrev) >= 10240) {
      unsigned long now = millis();
      size_t chunkSent = bodyLen - remaining - sentPrev;
      float speed = chunkSent * 1000.0f / (now - tPrev) / 1024.0f;
      Serial.printf("[Body] 进度 %d/%d bytes (%.1f%%), 本段%.1f KB/s\n",
                    (int)(bodyLen - remaining), (int)bodyLen,
                    (float)(bodyLen - remaining) * 100.0f / bodyLen, speed);
      sentPrev = bodyLen - remaining;
      tPrev = now;
    }
    yield();
  }
  Serial.printf("[Body] 发送 %d bytes, 耗时 %lums\n", (int)bodyLen, millis() - bStart);

  // 4. 读取响应 (用 read()+concat() 代替 readString(), 避免30秒流超时空等)
  //    策略: 总超时30s兜底 + 数据静止2s提前退出 (响应分块返回时, 数据流若静止2秒认为已读完)
  response = "";
  response.reserve(4096);
  uint8_t rbuf[1024];
  unsigned long rStart = millis();
  unsigned long lastData = millis();
  while (client.connected() || client.available()) {
    int avail = client.available();
    if (avail > 0) {
      int toRead = (avail < (int)sizeof(rbuf)) ? avail : (int)sizeof(rbuf);
      int n = client.read(rbuf, toRead);
      if (n > 0) {
        response.concat((const char*)rbuf, n);
        lastData = millis();
      }
    } else {
      yield();
      if (millis() - lastData > 2000) break;  // 数据静止2秒, 认为响应已读完
    }
    if (millis() - rStart > 30000) break;     // 总超时30秒兑底
  }
  client.stop();
  Serial.printf("[Resp] 读取 %d bytes, 耗时 %lums\n", (int)response.length(), millis() - rStart);

  // 5. 解析HTTP状态码 + 剥离响应头 + 解码chunked
  int code = -3;
  int firstLine = response.indexOf('\n');
  if (firstLine > 0) {
    String sl = response.substring(0, firstLine);
    sl.trim();
    int sp1 = sl.indexOf(' ');
    int sp2 = sl.indexOf(' ', sp1 + 1);
    if (sp1 > 0 && sp2 > sp1) code = sl.substring(sp1 + 1, sp2).toInt();
  }
  int sep = response.indexOf("\r\n\r\n");
  if (sep >= 0) response = response.substring(sep + 4);

  // 判断是否 chunked 编码: 响应头里包含"Transfer-Encoding: chunked"或原始body格式为"HEX\r\n...\r\n0"
  // 简单可靠的判断: 如果首行是纯十六进制数字(可能带;ext), 就是chunked
  {
    String raw = response;
    int nl = raw.indexOf("\r\n");
    if (nl > 0 && nl <= 10) {  // chunk size行很短 (<=0xFFFF, 4位16进制最多带扩展)
      String line1 = raw.substring(0, nl);
      line1.trim();
      bool isHex = line1.length() > 0;
      for (unsigned int i = 0; i < line1.length(); i++) {
        char c = line1.charAt(i);
        if (c == ';') break;               // chunk extension, 停止扫描
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          isHex = false; break;
        }
      }
      if (isHex) {
        // chunked 解码: 读SIZE\r\n, 拼SIZE字节, 重复直到SIZE==0
        String decoded;
        int pos = 0;
        while (pos < (int)raw.length()) {
          int szEnd = raw.indexOf("\r\n", pos);
          if (szEnd < 0) break;
          String szStr = raw.substring(pos, szEnd);
          szStr.trim();
          int semi = szStr.indexOf(';');
          if (semi >= 0) szStr = szStr.substring(0, semi);
          long chunkSz = strtol(szStr.c_str(), NULL, 16);
          pos = szEnd + 2;
          if (chunkSz == 0) break;
          if (pos + chunkSz > (int)raw.length()) {
            decoded += raw.substring(pos);
            break;
          }
          decoded += raw.substring(pos, pos + chunkSz);
          pos += chunkSz + 2;   // 跳过 chunk数据 + \r\n
        }
        response = decoded;
      }
    }
  }
  return code;
}
#endif  // !SERIAL_BRIDGE_MODE

