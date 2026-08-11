# STM32 节点 OTA 升级方案

> 基于参考例程《0-BootLoader区例程（节点板程序）》架构，移植到本项目。
> 适用节点: STM32F103C8T6（64KB Flash / 20KB RAM），网关: ESP8266。
> 文档日期: 2026-08-11

---

## 1. 目标与背景

节点固件无法拆机烧录，需要支持**远程升级**。升级链路：

```
OneNET平台(固件仓库)
   │  HTTP 下载(支持断点续传)
   ▼
ESP8266 网关 ──LoRa 128B/包 分包下发──? STM32 节点 BootLoader
                                               │ 写入内部Flash + 校验
                                               ▼
                                          跳转到新固件(APP)
```

- 网关负责：从平台下载固件 → 分包经 LoRa 下发 → 每包确认重发
- 节点负责：BootLoader 接收固件 → 写入 Flash → CRC 校验 → 跳转
- 网关自身升级另走 ArduinoOTA（局域网），不在本文档范围

---

## 2. 分区规划（F103C8T6 仅 64KB，单区方案）

```text
Flash 0x08000000 ─ 0x08001FFF   BootLoader   8KB   (页0~7,  常驻, 永不覆盖)
Flash 0x08002000 ─ 0x0800FBFF   APP 程序区   55KB  (页8~251)
Flash 0x0800FC00 ─ 0x0800FFFF   标志页       1KB   (页252,  升级标志/版本)
```

- **单区方案取舍**：升级中途断电会损坏 APP，但 BootLoader 永远在，重新升级即可恢复。演示/比赛场景可接受。
- **标志页用途**：APP 收到升级指令后写入升级标志（Flash 末页），复位后 BootLoader 据此决定"进 Boot 接收固件"还是"直接跳 APP"。
- F103C8T6 Flash 页大小 = 1KB（中容量），写 Flash 需按页擦除、2 字节(半字)编程。

### Keil 工程设置（APP 工程必须改）

| 项目 | 原值 | 新值 |
|------|------|------|
| IROM1 Start | 0x08000000 | **0x08002000** |
| IROM1 Size | 0x10000 (64K) | **0xDC00 (55K)** |
| 输出 .bin | 无 | fromelf --bin（见 5.4） |

---

## 3. 固件包格式（.bin 自定义文件头）

升级固件 = 12 字节文件头 + 代码段：

```c
typedef struct {
    uint16_t magic;      /* 0xA55A 魔数, 防误刷非固件文件 */
    uint16_t version;    /* 固件版本, 如 0x0102 = V1.2 */
    uint32_t length;     /* 代码段长度(字节) */
    uint32_t crc32;      /* 代码段 CRC32 */
} FwHeader_t;            /* 共12字节, 打包工具在 .bin 前拼接 */
```

- 版本校验：网关下发时带目标版本，BootLoader 校验 header.version 与目标一致才写入
- 完整性校验：接收完成后对代码段算 CRC32 与 header 比对，失败拒绝跳转

---

## 4. BootLoader 设计（新建独立工程）

### 4.1 启动分支流程

```c
main():
    初始化(时钟/USART2/LoRa/Flash/看门狗/按键)
    读取标志页:
    │
    ├─ 标志 = OTA_GO      → 进入 固件接收模式(Boot 停留, 收 LoRa 分包)
    ├─ 上电 2s 窗口内按键  → 进入 Boot 菜单(调试用, 可选)
    └─ 其他               → Load_APP() 跳转 APP
```

### 4.2 固件接收协议（LoRa 透传分包）

沿用 Xmodem 思想，链路换成 LoRa 定点传输（帧头 3B 地址 + 数据）：

```text
网关 → 节点: [地址头3B][0x02][包序号Hi][包序号Lo][128B数据][CRC16 Hi][CRC16 Lo]
节点 → 网关: [地址头3B][0x03] 收到正确 → ACK
             [地址头3B][0x15] 收到错误 → NAK(重发)
             [地址头3B][0x04] 接收完成 → EOT
             [地址头3B][0x18] 主动取消 → CAN
```

- 每包 128B 数据 + 2B CRC16-XMODEM（poly 0x1021，初值 0x0000，软件实现）
- 包序号 0~65535 循环，防丢包/乱序
- 网关每包最多等 1s ACK，超时重发，**每包最多 3 次重试**
- 总超时 **120s**（文档 2.8：OTA 期间网关暂停正常轮询）
- 帧头沿用 lora_node.h 已预留: `OTA_OK=0xD1` / `OTA_RETRY=0xE1`（节点→网关状态上报）

### 4.3 Flash 写入策略

- 接收前擦除 APP 区（页 8~251）
- 边收边写：半字编程（`FLASH_ProgramHalfWord`），写完立即 CRC 累计
- 接收完成 → 校验 CRC32 → 写标志页"升级成功" → 清 OTA_GO → 复位
- 校验失败 → 保持 OTA_GO，等待重新升级（不清除）

### 4.4 跳转代码（标准套路）

```c
#define APP_ADDR 0x08002000

typedef void (*pFunction)(void);

void Load_APP(uint32_t appxaddr)
{
    /* 栈顶地址合法性校验: 必须落在 RAM 区 */
    if (((*(volatile uint32_t*)appxaddr) & 0x2FFE0000) == 0x20000000)
    {
        __disable_irq();                       /* 关中断 */
        __set_MSP(*(volatile uint32_t*)appxaddr);              /* 重设栈指针 */
        pFunction jump = (pFunction)(*(volatile uint32_t*)(appxaddr + 4)); /* 复位向量 */
        jump();                                /* 跳转, 不返回 */
    }
}
```

### 4.5 关键依赖（移植自参考例程）

| 模块 | 例程(G0+HAL) | 本项目(F103+标准库) |
|------|--------------|---------------------|
| Flash 擦写 | HAL_FLASH | `FLASH_Unlock/ErasePage/ProgramHalfWord/Lock` |
| CRC | HAL_CRC(硬件) | 软件 CRC16-XMODEM + CRC32 |
| 传输 | USART Xmodem | USART2 + LoRa 定点传输 |
| 看门狗 | IWDG(HAL) | IWDG(标准库, 已在本项目实现) |

---

## 5. APP 工程改造（现有节点工程）

### 5.1 启动地址与向量表偏移

`main()` 最开头（时钟初始化之前）设置向量表偏移：

```c
SCB->VTOR = FLASH_BASE | 0x2000;   /* F103 支持 VTOR(Cortex-M3) */
```

### 5.2 升级触发（APP 侧）

收到网关 OTA 指令（如 `AT+OTA=start,version`）：

```c
1. 校验目标版本 > 当前版本(APP_VERSION 宏)
2. 擦写标志页, 写入 OTA_GO 标志 + 目标版本
3. NVIC_SystemReset();   /* 复位进入 BootLoader */
```

### 5.3 升级完成后恢复业务

- Boot 跳转 APP 后，APP 首个周期上报一次版本号（经 LoRa），网关确认"新版本已生效"上报平台
- 标志页"升级成功"标记由 Boot 在跳转前清除

### 5.4 生成 .bin（Keil）

Options → User → After Build → 添加：

```
fromelf --bin --output ..\..\Output\app.bin ..\..\Output\BH-F103.axf
```

编译后在 `Node/Output/` 得到 app.bin，配合打包工具（Python 脚本）拼接 12 字节文件头。

---

## 6. 网关端设计（ESP8266）

### 6.1 固件来源（二选一）

1. **OneNET 官方 OTA 服务（首选）**：`ota.heclouds.com`，支持版本上报/任务检测/断点续传(Range)/进度上报；控制台入口在产品 → 固件管理。鉴权用现有签名 token。
2. **设备文件管理（兜底）**：`device/file-upload` 上传固件，网关 `file-download?fid=xxx` 下载。入口已确认存在。

### 6.2 下载

- 新增 `ESP8266HTTPClient` 下载固件（网关现有代码仅 WiFiClient/MQTT，需加 HTTP）
- 支持 `Range` 头断点续传（与平台协议对应）
- 下载完成算 CRC32 → 拼 12B 文件头 → 生成待下发固件

### 6.3 下发调度

```c
OTA 流程(目标节点addr, 固件buf, len):
    1. 暂停正常轮询(所有节点), 进入 OTA 模式
    2. 发握手: 版本 + 长度 + CRC32
    3. 循环 i = 0..len/128:
       发第 i 包 → 等 ACK(1s)
       收到 NAK/超时 → 重发, 最多 3 次 → 失败则中止整个 OTA
    4. 发 EOT → 等节点"升级成功"上报(OTA_OK=0xD1)
    5. 恢复轮询
```

- OTA 期间只服务目标节点，其他节点数据用 PING 兜底（超时容忍）
- 总超时 120s，超时中止并上报平台失败原因

---

## 7. 完整升级时序

```text
用户/平台                      网关                         节点
   │ 创建升级任务(目标版本)      │                            │
   │───────────────────?        │                            │
   │                            │ HTTP下载固件+断点续传        │
   │                            │───────────────?(OneNET下载) │
   │                            │ LoRa下发: AT+OTA=start,V2   │
   │                            │───────────────────────────?│ 校验版本>当前
   │                            │                            │ 写OTA_GO标志
   │                            │?───────────────────────────│ (无回包, 直接复位)
   │                            │   (节点已重启进 BootLoader) │
   │                            │ 0x02 包0(128B+CRC16)──────?│ 写Flash
   │                            │?── ACK(0x03) ──────────────│
   │                            │ 0x02 包1 ... 包N(循环)      │
   │                            │?── EOT(0x04) ──────────────│ 全部收到
   │                            │?── OTA_OK(0xD1) ───────────│ CRC通过→跳转APP
   │                            │ 正常轮询 AT+DATA ─────────?│ 新固件运行
   │                            │?── 数据+版本(新) ──────────│
   │?───────────────────        │ 上报新版本, 任务完成         │
```

---

## 8. 异常处理

| 场景 | 处理 |
|------|------|
| 升级中途断电 | APP 损坏 → 复位进 Boot（OTA_GO 仍在）→ 重新升级可恢复 |
| 收到坏固件/CRC 失败 | 拒绝跳转，保持 Boot，等重发；网关上报失败 |
| 某包 3 次重试仍无 ACK | 中止 OTA，网关恢复轮询，上报失败原因 |
| 120s 总超时 | 中止，恢复业务；节点保持 OTA_GO，可续传（包序号从断点继续） |
| 误刷非固件文件 | magic≠0xA55A 拒绝写入 |
| 多节点同时在线 | OTA 只服务目标节点，其余暂停轮询（PING 超时容忍） |

---

## 9. 实施步骤（按依赖顺序）

1. **打包工具**：Python 脚本给 app.bin 拼 12B 文件头 + 算 CRC32（复用 Tool 目录）
2. **BootLoader 工程**（新建 Keil 工程）：跳转 + Flash 擦写 + LoRa 分包接收 + CRC16/32 + 标志页
3. **APP 工程改造**：IROM1 偏移 + VTOR + 升级触发指令 + bin 输出（编译产物不提交 git）
4. **单板验证**：烧 Boot + 旧 APP → 手动触发升级 → 跳转成功
5. **网关端**：HTTP 下载 + 分包下发 + 重试/超时 + OTA 期间暂停轮询
6. **平台接入**：OneNET 固件管理或文件管理上传固件，网关触发下载
7. **端到端测试**：平台下发 → LoRa 升级 → 跳转 → 新版本上报
8. **异常测试**：断点续传、坏固件、中途断电、超时中止

---

## 10. 验收标准

- [ ] 端到端升级成功：平台上传新固件 → 节点自动升级 → 新版本生效并上报
- [ ] 断点续传：升级中断后从断点继续，无需重头
- [ ] 坏固件拒绝：CRC/魔数错误不跳转，不损坏旧系统
- [ ] 中途断电可恢复：重新升级成功
- [ ] 升级后业务自动恢复：车位数据正常上报
- [ ] 升级不干扰其他节点：OTA 期间其他节点不掉线失联

---

## 附: 参考例程路径

```
g:\资源\单片机\STM32\LoRa模块+stm32开发板lora网关wifi\1-例程源码\1-例程源码lora\0-BootLoader区例程（节点板程序）
```
