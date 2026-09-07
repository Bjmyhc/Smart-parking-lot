# OTA 升级可靠性优化方案设计文档

> 适用范围：智能停车场物联网项目（STM32F103C8T6 节点 + LoRa 半双工链路 + ESP8266 网关 + OneNET 平台）
> 架构背景：Boot 0x08000000 / 配置区 0x08003000 / APP 0x08004000(47KB) / 升级标志页 0x0800FC00
> 升级流程：平台 SOTA → 网关下载固件 → AT+OTA 触发节点 → 节点回 ACK + 写标志 + 复位 → Boot 擦 APP → 收 Xmodem 分包 → CRC32 校验 → 跳 APP

---

## 1. 问题分析报告

### 1.1 故障现象

网关下发 `AT+OTA=start,<ver>` 后，节点已成功接收并回复 `AT+OTA:ack`，但该 ACK 因网络故障 / 信号干扰 / 网关处理异常而丢失。网关因此停止后续分包发送，而节点已擦除 APP 分区并进入等待分包状态，最终节点无法正常启动（"节点死亡"）。

### 1.2 根因链（"节点死亡"完整链路）

通过全链路代码排查（网关 `ota_handler.cpp` 状态机、`lora_handler.cpp` RX 分支、节点 Boot `main.c`、App `app_tasks.c`），确认根因链如下：

```
网关发 AT+OTA=start
  └─> 节点收到 → LoRa_Node_SendAck("AT+OTA:ack") → 200ms 后写标志+复位   [app_tasks.c:443-455]
        └─> ① 网关 ota_notifyTriggerAck() 全项目无调用点 ← 协议缺陷！
              lora_handler.cpp 的 LORA_FRAME_ACK 分支只匹配 ZombieThreshold/
              SetLed/SensorDistance/PONG，从不匹配 "AT+OTA:ack"
              → s_triggerAcked 永远为 false
              → OTA_TRIGGER_NODE 状态必然超时，重发 3 次后进 OTA_FAILED   [ota_handler.cpp:394-415]
              → 网关放弃，不再发任何分包
  └─> 节点侧此时已：
        └─> ② OTA_Process() 第一步就擦除整个 APP 区(47KB)，无论是否有固件数据   [main.c:OTA_EraseAppArea]
        └─> ③ 裸等 120s 首包超时 → 失败路径只打印 + 发 NAK，不写 OTA_FLAG_DONE  [main.c:616-620]
              → 标志页永远保持 OTA_FLAG_GO
        └─> ④ 下次复位(含 IWDG 看门狗复位)重进升级模式 → 再次擦除 APP → 无包可收
              → OTA_Process 返回后 main() 进入 while(1)喂狗死循环   [main.c:797-801]
              → 节点永久无法启动 = "节点死亡"
```

### 1.3 ACK 丢失原因分类

| 类别 | 具体原因 | 代码/机制佐证 |
|------|---------|--------------|
| **协议设计缺陷（直接根因）** | 网关侧 ACK 确认机制形同虚设：`ota_notifyTriggerAck()` 定义了但**无调用点**，节点回的 `AT+OTA:ack` 在 `LORA_FRAME_ACK` 分支无匹配项，被静默丢弃 | `lora_handler.cpp:303-340` 四个 strcmp 无一匹配 OTA；`ota_handler.cpp:672` 定义无调用 |
| **网络层/链路层** | LoRa 半双工冲突：节点回 ACK 时若网关正在 TX 则撞包丢失；433MHz 同频干扰、多径、距离衰减 | 网关发 AT+OTA 前仅等 500ms 信道空闲（`ota_handler.cpp:368-371`），无信道检测，无法保证节点回复时信道空闲 |
| **设备资源限制** | 节点 App 回 ACK 后仅延迟 **200ms** 即 `NVIC_SystemReset()`，LoRa 模块 9600bps 下 ACK 空中发送 + 模块转发延迟可能未完成，复位打断发送 | `app_tasks.c:453-455` |
| **时序窗口失配** | 节点总超时 120s < 网关总超时 180s：网关先放弃（进入 FAILED 后立即回 IDLE 不再发），节点还在裸等；且网关放弃后节点无任何感知手段 | `boot_cfg.h:99` vs `lora_protocol.h:OTA_TOTAL_TIMEOUT_MS` |
| **状态持久化缺陷** | Boot 失败不清 `OTA_FLAG_GO`，看门狗/上电复位反复触发"擦除→空等→超时"死循环 | `main.c:616-646` |

### 1.4 结构性缺陷清单

1. **触发握手失效**：`ota_notifyTriggerAck` 无调用点（必须修复，否则触发阶段永远失败）
2. **无两阶段提交**：擦除 APP 区发生在验证固件头之前，网关中途放弃即损坏旧 APP
3. **失败不清标志**：Boot 失败路径不写 `OTA_FLAG_DONE`，导致死循环
4. **无恢复路径**：`OTA_Process` 失败后 main() 裸 `while(1) 喂狗`
5. **无断点续传**：重进升级模式必然重擦 + 从头收，浪费且放大失败窗口
6. **无整链重试**：网关 `OTA_FAILED` 直接回 `OTA_IDLE`，一次性放弃
7. **复位时序过短**：200ms 不足以保证 ACK 空中发出

---

## 2. 优化目标

| 目标 | 指标 | 达成手段 |
|------|------|---------|
| 高可靠性 | 完整 OTA 成功率 ≥ 99.9% | 触发握手修复 + 指数退避重传 + 整链重试 + 断点续传 |
| 零死亡 | ACK 丢失场景下节点不死概率 100% | 两阶段提交（验证后才擦除）+ 失败清标志安全回退 |
| 快速恢复 | 失败后 ≤ 30s 恢复升级前工作状态 | 未擦除即失败 → 直接跳旧 APP（实测 <1s）；擦除后失败 → 保持可接收状态等网关重试 |
| 网络健壮 | 应对丢包/干扰/半双工冲突 | 重传幂等（同 seq 可重复收）+ 信道退避 + Boot 就绪握手 |

---

## 3. 技术方案

### 3.1 协议层改进

#### 3.1.1 新增控制字节（节点 Boot → 网关）

```c
#define OTA_READY  0x11   /* 节点进入升级模式后主动上报就绪 + 续传点 */
```

空中帧格式（LoRa 定点传输，`[AddrH][AddrL][CH]` + 数据）：

```
节点→网关 READY 帧:  [0x00][0x00][0x00] 0x11 [seqH][seqL][lenH][lenL]
                     └── 目标(网关)地址头 ──┘  └── 续传点: 已收最后一包序号 + 已写字节数 ──┘
```

- `seq=0, len=0`：全新升级，从第 1 包开始
- `seq=N`：断点续传，网关从第 N+1 包开始发送
- 该字节走 OTA 控制通道（`ota_feedByte` 直接字节识别），不进 `LORA_FRAME_*` 帧状态机，与现有 `ACK/NAK/CAN` 一致

#### 3.1.2 触发握手修复（核心）

`lora_handler.cpp` 的 `LORA_FRAME_ACK` 分支新增匹配：

```c
if (strncmp((char *)rxBuf, "AT+OTA", 6) == 0)
    ota_notifyTriggerAck();      /* 兼容 AT+OTA:ack / AT+OTA:version_ok */
```

- `AT+OTA:ack`：节点确认接收，进入复位等待阶段
- `AT+OTA:version_ok`：版本已最新无需升级，网关直接置 `OTA_COMPLETE`（平台可见"已是最新"而非"失败"）

#### 3.1.3 ACK 重传机制（指数退避 + 抖动）

| 层级 | 当前行为 | 改进后 |
|------|---------|--------|
| 触发命令重发 | 固定 2s × 3 次 | 退避 2s→4s→8s→16s，最多 5 次，加 ±20% 随机抖动防同频重发碰撞 |
| 数据包重发 | 固定 2s × 3 次 | 退避 2s→3s→5s→8s，最多 4 次，加 ±10% 抖动 |
| 重发幂等性 | 同 seq 重发节点丢弃重复包 | 保持（Xmodem 天然幂等），保证重传不破坏状态 |

重传间隔公式：`delay = base × 2^retry × (1 + 抖动)`，抖动 `rand()%20-10 %`。指数退避给链路自愈留出时间，避免同频节点同步重发互相干扰。

#### 3.1.4 Boot 就绪握手（替代固定定时等待）

- 节点进入 `OTA_Process()` 后**不再立即擦 APP**，而是先发 `OTA_READY(seq, len)` 上报
- 网关 `OTA_WAIT_NODE_RESET` 状态改为等待 READY 帧（15s 超时），收到后解析续传点并进入发包
- 网关 15s 未收到 READY → 判定节点未进 Boot（命令丢失/节点异常）→ 整链重试
- 收益：网关不再盲目按 `OTA_NODE_RESET_WAIT_MS(5s)` 定时发包，节点何时就绪网关何时发包，彻底消除"网关早发、节点未就绪"的窗口

#### 3.1.5 断点续传

- 节点每成功写入 N 包（取 **32 包 = 4KB**，平衡 Flash 擦写寿命与断电丢失量）持久化一次检查点 `seq` 到标志页后半段半字（`0x0800FC00 + 4` 偏移，半字编程，不覆盖 GO/DONE 标志位）
- 断电重启后 Boot 读检查点 → READY 帧上报 → 网关从 `seq+1` 续传；重复收到的包节点按 seq 幂等丢弃
- 47KB 固件全程最多擦写标志页 12 次，远低于 Flash 1 万次寿命，可接受

### 3.2 节点端安全保护机制（核心："节点不死"）

#### 3.2.1 两阶段提交（先验证固件头，再擦除 APP 区）

```
进入升级模式 → 发 READY(seq) → 等首包(15s)
  ├─ 收到首包 → 校验前 12B FwHeader_t(magic==0xA55A && length<=APP_MAX_SIZE)
  │     ├─ 有效 → 此刻才擦除 APP 区 → 回写首包 → 继续收分包
  │     └─ 无效 → 发 NAK → 清标志 → 直接 Load_APP(旧 APP 完好无损!)   ← 关键
  └─ 15s 内无首包 → 发 NAK → 清标志 → Load_APP(旧 APP 完好无损!)
```

**收益**：所有"网关放弃"场景（ACK 丢失、触发失败、中途断链）发生时，若节点尚未收到有效首包，旧 APP 从未被擦除，节点可瞬时回退，恢复时间 <1s。

#### 3.2.2 失败安全回退（统一入口 `OTA_AbortRecovery()`）

所有失败路径（首包超时/头无效/收 CAN/CRC 失败/总超时）统一执行：

```
OTA_AbortRecovery():
  1. OTA_WriteFlag(OTA_FLAG_DONE)      // 清除 GO 标志，防死循环（核心修复②）
  2. OTA_SendResp(NAK)                 // 通知网关
  3. 若 APP 区未被擦除 → Load_APP(APP_ADDR)          // 瞬时恢复旧 APP
  4. 若 APP 区已擦除 → 保持升级模式(见 3.2.3)，等网关整链重试/续传
```

#### 3.2.3 擦除后失败的兜底（安全等待态，非死亡）

擦除后失败意味着旧 APP 已不存在，无法回退。此时节点：

- 保持升级模式，每 10s 发一次 NAK 催促网关重发，总等待 180s（与网关总超时对齐）
- 180s 后仍无数据 → 写 DONE 清标志 → 复位 → Boot 发现无 GO 且 APP 区无效 → **再次进入升级模式继续等待**（保持可接收状态）
- 该状态是"安全等待"而非"死亡"：网关/平台随时可重新触发升级（配合断点续传快速恢复）。配合网关整链重试（3 次）+ 平台重触发，最终收敛到成功或人工介入，**不会出现"永久无法启动"**。

#### 3.2.4 超时参数汇总

| 参数 | 当前值 | 改进值 | 说明 |
|------|-------|--------|------|
| 首包等待 | 无(直接等120s) | **15s** | 两阶段提交下首包超时即可回退旧 APP |
| 每包等待 | 1s | 1s（保持） | Xmodem 每包超时 |
| 总超时 | 120s | 180s | 与网关对齐，避免"网关还活着节点先放弃" |
| 擦除后催发间隔 | 无 | 10s 发一次 NAK | 主动催促网关重发 |
| 检查点持久化 | 无 | 每 32 包 | 断点续传 |

### 3.3 网关端控制逻辑优化

#### 3.3.1 状态机修改

```
改进后网关状态机:
OTA_IDLE → OTA_TRIGGER_NODE(指数退避重发≤5次, 收到 ack→下一步 / version_ok→COMPLETE)
  → OTA_WAIT_NODE_READY(15s, 等 READY 帧, 解析续传 seq; 超时→FAILED)
  → OTA_DOWNLOADING(从 seq+1 开始) → OTA_SENDING → OTA_WAIT_ACK(退避重发≤4次)
  → OTA_SEND_EOT → OTA_WAIT_FINAL_ACK(超时视为完成, 保持)
  → OTA_COMPLETE / OTA_FAILED(整链重试≤3次后真失败)
```

#### 3.3.2 整链自动重试

- `OTA_FAILED` 分支增加整链重试计数 `s_chainRetry`：每次失败后重新从 `OTA_TRIGGER_NODE` 发起，最多 3 次
- 重试间间隔 5s，给链路/节点复位留出时间
- 3 次全部失败才回 `OTA_IDLE` 并向平台上报失败（`onenet_ota.cpp` 上报 202）
- 平台侧 `OTA_PLAT_FINISH` 的判定从"网关一次性结果"改为"网关整链重试最终结果"

#### 3.3.3 保留现有防护

- OTA 期间暂停轮询/控制命令发送（`lora_handler.cpp:568-575`），保半双工空口
- 最终 ACK 超时视为完成（防尾部卡死）

### 3.4 通信时序优化

| 项 | 当前 | 改进 | 理由 |
|----|------|------|------|
| App 回 ACK 后复位延迟 | 200ms | **500ms** | 保证 ACK 在 9600bps 半双工链路上完成空中发送（含模块转发延迟） |
| 网关发 AT+OTA 前信道等待 | 500ms | 500ms（保持）+ 重发时同样等待 | 防半双工冲突 |

---

## 4. 实施步骤

### 4.1 协议改进详细设计

**新增宏（网关 `lora_protocol.h` / 节点 `boot_cfg.h` 同步）：**

```c
#define OTA_READY                 0x11   /* 节点就绪上报 */
#define OTA_FIRST_PKT_TIMEOUT_MS  15000  /* 首包等待(节点) */
#define OTA_READY_WAIT_MS         15000  /* READY 等待(网关) */
#define OTA_ERASED_NAK_INTERVAL_MS 10000 /* 擦除后催发间隔 */
#define OTA_CHAIN_MAX_RETRY       3      /* 整链重试次数(网关) */
#define OTA_TRIGGER_MAX_SEND_NEW  5      /* 触发命令重发上限 */
#define OTA_CHKPT_INTERVAL_PKT    32     /* 检查点持久化间隔(包) */
```

**状态机定义（节点 Boot 侧）：**

```
节点升级子状态:
READY_SENT → WAIT_FIRST(15s) ──首包有效──> ERASE_APP → WRITE_BACK_FIRST → WAIT_DATA(180s)
                │                                   ↑                    │
                ├─超时/头无效 → ABORT: 清标志 + Load_APP(旧APP)          │
                └────────────── 保持 READY 重发(每5s) 直到收到首包 ──────┘
WAIT_DATA: 每包1s超时重试; 每10s NAK催发; 总180s超时 → 清标志 → 复位 → 重进升级模式(安全等待)
```

### 4.2 网关端修改清单

| 文件 | 修改内容 |
|------|---------|
| `lora_handler.cpp` | ① ACK 分支新增 `AT+OTA` 前缀匹配 → 调 `ota_notifyTriggerAck()`（**必改，触发握手修复**）② RX 字节流新增 `OTA_READY(0x11)` 识别 → 调 `ota_notifyNodeReady(seq, len)` ③ 新增调试宏 `SIMULATE_ACK_LOSS`/`SIMULATE_PKT_LOSS_PERCENT` 模拟丢包 |
| `ota_handler.cpp` | ① `OTA_TRIGGER_NODE`：指数退避重发（2s→4s→8s→16s，≤5 次）；`version_ok` 直接完成 ② `OTA_WAIT_NODE_RESET` 改为 `OTA_WAIT_NODE_READY`：15s 等 READY 帧，解析续传 seq ③ 发包起点改为 `s_readySeq + 1` ④ `OTA_WAIT_ACK` 指数退避 ≤4 次 ⑤ `OTA_FAILED` 增加整链重试（≤3 次，间隔 5s）⑥ 新增 `ota_notifyNodeReady()` 实现 |
| `ota_handler.h` | 新增 `ota_notifyNodeReady()` 声明、`OTA_READY` 宏、`s_readySeq`/`s_chainRetry` 变量声明 |
| `lora_protocol.h` | 新增 `OTA_READY 0x11` 等协议宏 |

### 4.3 节点端修改清单

| 文件 | 修改内容 |
|------|---------|
| `boot_cfg.h` | 新增 4.1 节协议宏（`OTA_READY`、`OTA_FIRST_PKT_TIMEOUT_MS`、`OTA_ERASED_NAK_INTERVAL_MS`、`OTA_CHKPT_INTERVAL_PKT` 等） |
| `main.c`(Boot) | ① `OTA_Process()` 重构：进升级模式先发 `OTA_READY(lastSeq)`；首包验证通过后才擦 APP（两阶段提交）；失败统一走 `OTA_AbortRecovery()` ② 新增 `OTA_AbortRecovery()`：清标志 + 未擦除则 Load_APP / 已擦除则进入安全等待 ③ 新增检查点持久化（每 32 包写 `0x0800FC00+4` 半字）④ 新增擦除后 NAK 催发定时器 ⑤ `main()`：`OTA_Process` 返回后若 APP 无效则重进升级模式（安全等待），去掉裸 `while(1)喂狗` 死循环 |
| `app_tasks.c` | 复位延迟 200ms → 500ms（保证 ACK 空中完成） |

### 4.4 测试工具开发

1. **丢包模拟开关（网关侧，编译期）**
   ```c
   /* lora_handler.cpp 顶部 */
   #define SIMULATE_ACK_LOSS           1    /* 1=模拟 ACK 全部丢失 */
   #define SIMULATE_PKT_LOSS_PERCENT   30   /* 数据包随机丢包率% (0=关闭) */
   ```
   打开 `SIMULATE_ACK_LOSS`：收到节点 ACK 后直接丢弃不处理，验证节点不死亡。
   打开 `SIMULATE_PKT_LOSS_PERCENT`：发包前按概率直接不发，验证重传与续传。

2. **串口日志分析脚本**（Python，`Tool/ota_log_analyzer.py`）：
   解析网关/节点 USART 日志，自动统计：触发次数、每包重传次数分布、续传点、总耗时、成败结论，生成报告

3. **故障注入清单**：ACK 丢、首包丢、中段丢包、EOT 丢、升级中途断电重启（验证续传）

### 4.5 测试方案

- **功能测试**：20+ 用例覆盖正常升级、各故障注入点、version_ok、断电续传
- **压力测试**：连续 50 次完整升级（含 30% 丢包注入），统计成功率与平均耗时
- **恢复时间测量**：节点日志打点，从失败判定到 `Load_APP` 毫秒级计时，验证 ≤30s

---

## 5. 验收标准

| # | 验收项 | 测试方法 | 目标值 |
|---|--------|---------|--------|
| 1 | ACK 丢失节点安全恢复 | `SIMULATE_ACK_LOSS=1` 下触发升级，观察节点 | 节点 15s 内回退旧 APP 正常运行，**0 死亡** |
| 2 | 触发握手修复 | 正常升级，观察网关日志 | 收到 `AT+OTA:ack` 后进入复位等待，不再超时重发 |
| 3 | 完整升级成功率 | 50 次连续升级（含 30% 丢包注入） | 成功率 ≥ 99.9%（50/50 成功） |
| 4 | 失败恢复时间 | 故障注入后节点日志计时 | 未擦除失败 ≤ 30s（预期 <1s）；擦除后失败保持可接收态 |
| 5 | 断点续传 | 升级中途断电重启 | 从检查点续传，不从头开始，最终升级成功 |
| 6 | 看门狗防死循环 | 首包超时后复位 | 不再重复擦 APP，退出升级模式或安全等待 |
| 7 | 平台联动 | 网关整链 3 次重试耗尽 | 上报 202 失败，平台可重新触发 SOTA |

**测试报告交付物**：测试用例表（用例编号/步骤/预期/实际/结论）、测试环境（硬件连接、LoRa 参数、丢包配置）、压力测试结果统计、日志附件。

---

## 附录：修复优先级与依赖关系

```
P0（必须，解决"节点死亡"）:
  A. 网关 lora_handler.cpp 触发 ACK 匹配修复（ota_notifyTriggerAck 调用）
  B. 节点 Boot 失败路径写 OTA_FLAG_DONE（OTA_AbortRecovery 清标志）
  C. 节点 Boot 两阶段提交（先验证固件头再擦 APP）
  D. 节点 main() 去掉裸 while(1) 死循环（APP 无效重进升级模式）
P1（可靠性增强）:
  E. Boot READY 就绪握手（替代固定 5s 定时）
  F. 指数退避重传（触发/数据包两级）
  G. 网关整链重试 ≤3 次
  H. 复位延迟 200ms→500ms
P2（锦上添花）:
  I. 断点续传（检查点持久化 + READY 上报 seq）
  J. 测试工具（丢包模拟 + 日志分析）
```
