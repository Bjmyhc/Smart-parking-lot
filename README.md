# 智能停车场系统

> STM32 节点 + LoRa 无线 + ESP8266/ESP32 网关 + OneNET 云平台 + Flutter App 监控
> 多车位远程检测与控制的完整物联网解决方案

## 架构

```
STM32节点(超声波+地磁+LoRa) <-LoRa定点-> ESP8266/ESP32网关 <-MQTT-> OneNET云平台 <-HTTP-> Flutter App
```

每车位一个 STM32 节点（超声波 + 地磁），通过 LoRa 433MHz 定点传输与 ESP8266/ESP32 网关通信；
网关汇聚数据后以 MQTT（网关+子设备模式）代子设备上报 OneNET 云平台，App 页面实时展示并可远程控制。

## 功能特性

- **多车位检测**：超声波 + 地磁双重判定，车位三态（空闲 / 有车 / 僵尸车）
- **LoRa 定点传输**：433MHz 远距离，网关轮询防碰撞，二进制结构体直传
- **OneNET 网关+子设备模式**：代子设备上线 / 上报 / 下行命令路由
- **远程控制**：平台/App 下发 SetLed 等服务命令，节点执行并回 ACK 确认
- **OTA 远程升级**：STM32 节点 Xmodem OTA，Boot 两阶段提交 + 断电自愈；网关支持 FOTA（自身）/SOTA（节点）
- **自动恢复**：WiFi 断线自动重连、节点离线指数退避探测、插电自动上线
- **AP+Web 配网**：首次上电自动进入配网，配置持久化到 LittleFS
- **OLED 集中显示**：网关屏显示节点在线状态 / 车位状态 / 停车时长
- **App 监控**：Flutter 端实时数据展示 + 在线判定 + LED 远程开关
- **MCP 工具**：`Mcp_Tool/` 封装 OneNET API，支持大模型工具调用查询与控制

## 目录结构

| 目录 | 说明 |
|------|------|
| `Gateway/` | 网关程序（Arduino，ESP8266/ESP32 双平台，LoRa 轮询 + OneNET MQTT） |
| `Node/` | STM32 节点工程（BootLoader/APP/BSP，Keil MDK） |
| `User/` | Flutter APP 端（Android/iOS/Web，Provider 状态管理） |
| `shared/` | 网关与节点共用的 LoRa 协议头定义 |
| `Doc/` | 项目文档合集（架构设计 / 开发记录 / APP 开发 / 备赛指南等） |
| `Mcp_Tool/` | MCP 工具（Python，OneNET API 封装） |
| `PlateRecognition/` | 摄像头车牌识别参考代码 |
| `Tool/` | GBK 中文还原脚本等开发工具 |

## 快速上手

按顺序执行（详见 [Doc/项目说明.md](Doc/项目说明.md)）：

1. LoRa 模块 AT 指令配置（定点传输 / 信道 / 波特率 / 地址）
2. 网关烧录（Arduino IDE，配网后连 WiFi）
3. 节点烧录（Keil MDK，每个节点独立地址）
4. OneNET 平台创建产品 / 设备 / 子设备拓扑
5. 上电验证：数据上报 + 远程控制 + 掉线恢复

## 文档

| 文档 | 内容 |
|------|------|
| [Doc/项目说明.md](Doc/项目说明.md) | 项目概览、硬件接线、完整上手指南（**新人入口**） |
| [Doc/系统设计文档.md](Doc/系统设计文档.md) | 技术架构、通信协议、数据流、设计决策 |
| [Doc/开发文档.md](Doc/开发文档.md) | 开发阶段计划、踩坑记录、开发日志 |
| [Doc/APP开发文档.md](Doc/APP开发文档.md) | Flutter APP 端开发说明（v3.0） |
| [Doc/OTA升级方案.md](Doc/OTA升级方案.md) | STM32 节点 OTA 远程升级方案 |
| [Doc/OTA升级可靠性优化方案.md](Doc/OTA升级可靠性优化方案.md) | OTA 两阶段提交 + 断电自愈优化设计 |
| [Doc/技术难点分析报告.md](Doc/技术难点分析报告.md) | 通信可靠性 / 空口占用 / 证书持久化等技术难点分析 |
| [Doc/通信架构重构方案.md](Doc/通信架构重构方案.md) | 通信架构重构设计 |
| [Doc/项目优化评估报告.md](Doc/项目优化评估报告.md) | 项目优化项与评估结论 |
| [Doc/省级物联网大赛备赛指南.md](Doc/省级物联网大赛备赛指南.md) | 比赛分析、答辩话术、PPT 建议 |
| [Doc/技术待办-延后事项.md](Doc/技术待办-延后事项.md) | 暂缓实施的技术改进项清单 |
| [Doc/硬件资料/百度官方api文档.md](Doc/硬件资料/百度官方api文档.md) | 百度车牌识别 API 参考文档 |

## License

仅供学习参考。
