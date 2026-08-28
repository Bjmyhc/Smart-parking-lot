# 摄像头 CapturePlate 服务下发 备用方案

> 生效时机：**确定比赛演示需要自动化调度（APP自动下发拍照、一次调用同步拿最终结果）时才启用。**
> 当前状态：手动 BOOT 按钮触发为主时，不需要启用，保持现有属性上报方案即可。
> 原则：双轨并行。启用服务下发后，**现有属性上报 / BOOT 按钮 / 三态闸门机制全部保留不动**，纯增量加功能。

---

## 一、OneNET Studio 服务配置（两选一）

### 方式 A：JSON 一键导入（最快，推荐）

直接把下面这段粘到你物模型导出文件里的 **`"services": []` 数组**中（现在物模型里 services 应该是空的，直接粘进去就行）：

```json
{
  "identifier": "CapturePlate",
  "name": "触发拍照识别",
  "description": "同步触发摄像头拍照并识别车牌，调用方阻塞等最终结果出参",
  "callType": "sync",
  "required": false,
  "inputParams": [
    {
      "identifier": "TargetSpot",
      "name": "目标车位号",
      "dataType": "string",
      "dataSpecs": {
        "length": 32
      },
      "description": "可选项：关联的车位编号（如P01），用于自动调度时记录取证对应哪个车位，留空也可",
      "required": false
    }
  ],
  "outputParams": [
    {
      "identifier": "ResultCode",
      "name": "结果码",
      "dataType": "int32",
      "dataSpecs": {
        "min": 0,
        "max": 20,
        "step": 1
      },
      "description": "0=识别成功；1=设备忙；2=拍照失败；3=JPEG失败；4=OCR网络/Token失败；5=OCR JSON错误；6=无车牌",
      "required": true
    },
    {
      "identifier": "PlateNumber",
      "name": "车牌号码",
      "dataType": "string",
      "dataSpecs": {
        "length": 16
      },
      "description": "识别成功返回车牌，失败返回空字符串",
      "required": false
    },
    {
      "identifier": "PlateColor",
      "name": "车牌颜色",
      "dataType": "string",
      "dataSpecs": {
        "length": 16
      },
      "description": "识别成功返回中文颜色，失败返回空字符串",
      "required": false
    },
    {
      "identifier": "PlateConfidence",
      "name": "识别置信度",
      "dataType": "float",
      "dataSpecs": {
        "min": 0,
        "max": 1,
        "step": 0.01
      },
      "description": "范围0-1，越大越可信，失败返回0",
      "required": false
    },
    {
      "identifier": "CaptureTime",
      "name": "拍照时间",
      "dataType": "string",
      "dataSpecs": {
        "length": 32
      },
      "description": "格式 YYYY-MM-DD HH:MM:SS",
      "required": false
    },
    {
      "identifier": "ResultMsg",
      "name": "结果描述",
      "dataType": "string",
      "dataSpecs": {
        "length": 128
      },
      "description": "成功=OK，失败=详细原因",
      "required": true
    }
  ]
}
```

### 方式 B：手动界面填写（手填步骤）

OneNET 控制台 → 产品 4enONCu0Y7 → 物模型 → 新建服务：

| 字段 | 填值 |
|---|---|
| 服务功能名（name） | `触发拍照识别` |
| 标识符（identifier） | `CapturePlate` |
| 调用方式 | **同步**（必选，APP才能同步拿结果） |
| 描述 | `同步触发拍照识别，返回最终车牌结果` |

然后点「添加入参」→ 1 个（可选）：
| 入参名 | 标识符 | 数据类型 | 长度 | 必填 |
|---|---|---|---|---|
| 目标车位号 | TargetSpot | string | 32 | 否 |

然后点「添加出参」→ 6 个（ResultCode 和 ResultMsg 必填，其他选填）：
| 出参名 | 标识符 | 类型 | 范围/步长 | 必填 |
|---|---|---|---|---|
| 结果码 | ResultCode | int32 | 0~20 步1 | ✅ 是 |
| 车牌号码 | PlateNumber | string | 16 | 否 |
| 车牌颜色 | PlateColor | string | 16 | 否 |
| 置信度 | PlateConfidence | float | 0~1 步0.01 | 否 |
| 拍照时间 | CaptureTime | string | 32 | 否 |
| 结果描述 | ResultMsg | string | 128 | ✅ 是 |

保存即可。

---

## 二、代码改动大纲（到时候改代码照这个清单走）

> 改动文件：**只改 `PlateRecognition/PlateRecognition.ino` 一个文件**
> 关键坑：**`invoke_reply` 只能发一次** → 所有流程必须跑完再一次性回

---

### ▶ 步骤 1：宏定义区加 4 组新常量（`DEVSTAT_FAIL` 定义附近加）

```cpp
// -------- CapturePlate 服务下发(OneNET call-service) 相关宏 --------
#define ONENET_SERVICE_CAPTURE   "CapturePlate"   // 服务identifier, 必须与物模型一致
// MQTT 服务 invoke 订阅: 通配+, 支持以后扩展其他服务 (必须写死设备路径, 不可含#多段通配)
#define ONENET_TOPIC_SERVICE_INVOKE  "$sys/" ONENET_PID "/" ONENET_DEVNAME "/thing/service/+/invoke"
// MQTT 服务 reply 主题模板: printf 注入msgId和serviceId, 格式严格同网关子服务reply机制
#define ONENET_TOPIC_SERVICE_REPLY_FMT "$sys/" ONENET_PID "/" ONENET_DEVNAME "/thing/service/%s/invoke_reply"
// 服务出参 ResultCode 枚举 (必须与物模型outputParams描述一致)
#define SRV_OK            0   // 0: 识别成功 (有车牌结果)
#define SRV_ERR_BUSY      1   // 1: 设备忙 recognizeInProgress=true (立即拒绝)
#define SRV_ERR_CAMERA    2   // 2: 摄像头拍照失败 (fb==NULL)
#define SRV_ERR_JPEG      3   // 3: JPEG编码失败
#define SRV_ERR_OCR_HTTP  4   // 4: 百度OCR HTTP失败 (WiFi/Token/百度侧)
#define SRV_ERR_OCR_JSON  5   // 5: OCR响应JSON解析失败
#define SRV_ERR_NO_PLATE  6   // 6: OCR words_result为空 (无车牌)
```

---

### ▶ 步骤 2：加全局「识别结果缓存」+ 订阅 invoke 主题

**2a. 全局变量区（`mqttWaitStartMs` 附近加）**：
```cpp
// CapturePlate 服务结果缓存: BOOT/属性/服务触发跑完 recognizePlate 后都写一份到这里, 服务reply直接读
typedef struct {
  int  resultCode;        // SRV_* 枚举值
  char plate[16];         // 车牌号码 (空="")
  char colorZh[16];       // 车牌颜色中文 (空="")
  float confidence;       // 0-1
  char captureTime[32];   // YYYY-MM-DD HH:MM:SS
  char msg[128];          // 成功="OK", 失败=详细原因
} CaptureResult_t;
CaptureResult_t gLastResult;   // 最近一次识别结果 (所有触发源共享)
```

**2b. `onenetMqttEnsureConnected` 函数中订阅行后面加第 3 个订阅**：
```cpp
bool sub3 = mqttClient.subscribe(ONENET_TOPIC_SERVICE_INVOKE);
// 然后把打印行的 sub1/sub2 改成 sub1/sub2/sub3:
Serial.printf("[MQTT] 已连接, 订阅: post/reply=%d, prop/set=%d, service=%d\n",
              sub1?1:0, sub2?1:0, sub3?1:0);
```

---

### ▶ 步骤 3：`recognizePlate` 里每个出口写一份结果到 gLastResult（不破坏现有 void 签名）

在**所有 return 之前**（成功 + 8 个失败出口，共 9 处），加 `cacheResult(...)` 写缓存：

| 出口位置（现有错误类型） | 填缓存代码 |
|---|---|
| 拍照失败 | `cacheResult(SRV_ERR_CAMERA, NULL, NULL, 0.0f, "摄像头拍照失败");` |
| JPEG失败 | `cacheResult(SRV_ERR_JPEG, NULL, NULL, 0.0f, "JPEG编码失败");` |
| Base64失败 | `cacheResult(SRV_ERR_OCR_HTTP, NULL, NULL, 0.0f, "Base64编码内存不足");` |
| Token失败 | `cacheResult(SRV_ERR_OCR_HTTP, NULL, NULL, 0.0f, "获取百度Token失败");` |
| Body分配失败 | `cacheResult(SRV_ERR_OCR_HTTP, NULL, NULL, 0.0f, "HTTP Body内存不足(PSRAM?)");` |
| OCR HTTP失败 | `cacheResult(SRV_ERR_OCR_HTTP, NULL, NULL, 0.0f, "OCR请求全部失败");` |
| JSON错误 | `cacheResult(SRV_ERR_OCR_JSON, NULL, NULL, 0.0f, "OCR响应JSON解析失败");` |
| 无车牌 | `cacheResult(SRV_ERR_NO_PLATE, NULL, NULL, 0.0f, "未识别到车牌(words_result为空)");` |
| 成功出口 | `cacheResult(SRV_OK, plate.c_str(), color, avgProb, capTime.c_str(), "OK");` |

补 1 个工具函数 `cacheResult`（放 `reportDeviceStatus` 上面），内部用 `strncpy` 安全拷贝字符串。

---

### ▶ 步骤 4：mqttCallback 加 service/invoke 分支（现有 prop/set 后面）

在 `mqttCallback` 里追加 **第 3 类 topic 分支**：

```cpp
// --- 3. thing/service/+/invoke: 平台APP下发服务调用 (CapturePlate 同步触发) ---
if (strstr(topic, "thing/service/") && strstr(topic, "/invoke")) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, (char*)payload);
  if (err) { Serial.printf("[MQTT] service/invoke 解析失败: %s\n", err.c_str()); return; }

  const char* msgId  = doc["id"]    | "";
  // 从topic里抽出服务identifier (取service/和/invoke之间的那段)
  char serviceId[32]; serviceId[0] = 0;
  const char* p = strstr(topic, "/thing/service/");
  if (p) {
    p += 15; // 跳过"/thing/service/"
    const char* q = strstr(p, "/invoke");
    if (q && (q - p) < 32) { memcpy(serviceId, p, q-p); serviceId[q-p] = 0; }
  }

  // [分支A] 设备忙 → 立刻reply SRV_ERR_BUSY (一次回完)
  if (recognizeInProgress) {
    srvInvokeReply(msgId, serviceId, SRV_ERR_BUSY, NULL, NULL, 0.0f, NULL, "设备忙, 请稍后重试");
    return;
  }

  // [分支B] CapturePlate服务 → 跑识别流程 → 缓存结果 → 一次性reply
  if (strcmp(serviceId, ONENET_SERVICE_CAPTURE) == 0) {
    recognizeInProgress = true;
    char triggerDesc[64]; snprintf(triggerDesc, sizeof(triggerDesc),
                                   "服务下发 CapturePlate (msgId=%s)", msgId);
    recognizePlate(triggerDesc);   // 阻塞跑完整流程 (拍照→OCR→属性上报→补0)
    recognizeInProgress = false;
    // 从gLastResult读最终结果 → 一次性invoke_reply
    srvInvokeReply(msgId, serviceId,
                   gLastResult.resultCode,
                   gLastResult.plate[0] ? gLastResult.plate : NULL,
                   gLastResult.colorZh[0] ? gLastResult.colorZh : NULL,
                   gLastResult.confidence,
                   gLastResult.captureTime[0] ? gLastResult.captureTime : NULL,
                   gLastResult.msg);
  }
  // 其他服务(以后扩展)在这里加分支...
}
```

---

### ▶ 步骤 5：加 `srvInvokeReply` 工具函数 + 调 OCR 重试次数

**5a. `srvInvokeReply` 工具函数（和 `uploadToOneNET` 并列放就行）**：
```cpp
// 封装并发送 invoke_reply. ⚠️ 只能调一次!!! 所有流程必须跑完再一次性回
static void srvInvokeReply(const char* msgId, const char* serviceId,
                           int resultCode, const char* plate, const char* colorZh,
                           float confidence, const char* capTime, const char* msg)
{
  if (!onenetMqttEnsureConnected()) { Serial.println("[MQTT][服务] 未连接, reply放弃"); return; }
  char topic[192];
  snprintf(topic, sizeof(topic), ONENET_TOPIC_SERVICE_REPLY_FMT, serviceId);

  StaticJsonDocument<768> oneDoc;
  oneDoc["id"]   = msgId;
  oneDoc["code"] = 200;            // 通信层永远=200, 业务结果在ResultCode里
  JsonObject out = oneDoc.createNestedObject("data");
  out["ResultCode"]       = resultCode;
  out["ResultMsg"]        = msg ? msg : "";
  out["PlateNumber"]      = plate ? plate : "";
  out["PlateColor"]       = colorZh ? plateColorZh(colorZh) : "";
  if (confidence > 0.001f) out["PlateConfidence"].set<float>(confidence);
  else                     out["PlateConfidence"].set<float>(0.0f);
  out["CaptureTime"]      = capTime ? capTime : getTimeString();

  size_t payloadLen = serializeJson(oneDoc, mqttTxBuf, MQTT_PACKET_BUF_SIZE);
  if (payloadLen == 0) { Serial.println("[MQTT][服务] reply序列化失败"); return; }
  Serial.printf("[MQTT][服务] %s 回包 code=200 ResultCode=%d (%s)\n",
                serviceId, resultCode, resultCode == SRV_OK ? "成功" : "失败");
  mqttClient.publish(topic, (const uint8_t*)mqttTxBuf, payloadLen, false);
  mqttClient.loop();
}
```

**5b. OCR 重试次数改 1（防止服务调用超时）**：
把 `OCR_RETRY_MAX` 从 2 → 1，最大总耗时 3 秒左右，稳稳落进平台服务同步超时窗口（一般 5 秒）。

**5c. APP 端（Flutter）配套注意事项**：
```
- call-service HTTP 客户端超时 → 建议设 8000ms (略大于平台5s超时 + 网络波动)
- 失败判断优先级:
  ① HTTP code != 200 → 网络 / OneNET 平台问题
  ② HTTP code == 200 但 data.ResultCode != 0 → 按 ResultMsg 提示展示失败原因
  ③ ResultCode == 0 → 成功，直接取 6 个出参展示
```

---

## 三、验证清单（改完代码按顺序测）

1. ✅ BOOT 按钮拍照 → 属性上报正常、日志 4 行格式不变、闸门 0 态正常
2. ✅ 属性下发 CaptureCmd=1 → 原有流程正常工作，并发锁生效
3. ✅ OneNET 服务测试台调 CapturePlate → HTTP 响应体拿到 ResultCode=0 + 完整 6 出参
4. ✅ 连续快速调 3 次服务 → 第 2~3 次拿到 ResultCode=1（设备忙拒绝生效）
5. ✅ 遮挡镜头模拟无车牌 → ResultCode=6 + ResultMsg 明确说明原因
6. ✅ 断开百度 Token 模拟失败 → ResultCode=4 + ResultMsg 说明 Token 获取失败
