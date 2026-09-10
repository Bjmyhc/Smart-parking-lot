/****************************************************************************
 * LoRa 节点通信驱动 - bsp_lora.h
 *
 * 功能描述:
 *   升级现有 LoRa 透传模块为定点传输驱动
 *   实现节点与网关之间的双向通信:
 *   - 响应网关轮询(证书查询/数据查询)
 *   - 发送传感器数据(结构体打包)
 *   - 接收下行控制命令(LED使能等)
 *
 * 硬件配置:
 *   USART2 (PA2/TX, PA3/RX) - LoRa 模块通信串口
 *   定点传输模式: [AddrH][AddrL][CH] + 数据
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-07
 ****************************************************************************/

#ifndef __LORA_NODE_H
#define __LORA_NODE_H

#include <stdint.h>
#include <stddef.h>   /* offsetof (CRC 计算用) */
#include "stm32f10x.h"

/* ⭐ S11 协议单一事实源: 帧头常量/定点地址/协议版本/CRC16/数据结构
 * 统一由共享头 lora_protocol.h 提供 (shared/ 目录, 与网关强制一致) */
#include "lora_protocol.h"

/* ==================== LoRa 通信参数 ====================
 * 节点身份(产品ID/设备名)由 **Boot 首次启动**固化到 Flash 配置区,
 * App 端统一从 node_config.h 的运行时变量引用:
 *   - g_nodeProductKey / g_nodeDeviceName
 * 身份默认宏位于 BootLoader/User/boot_cfg.h (NODE_PRODUCT_KEY/NODE_DEVICE_NAME)
 * 空中寻址使用 LoRa 模块硬件地址(ADDH/ADDL, 烧录时人工配置), 与代码无关.
 * 本头文件只保留 LoRa 通信相关参数
 * (LORA_GATEWAY_ADDR / LORA_CHANNEL 已由共享头提供) */
#define LORA_BAUD           9600        /* LoRa 串口波特率，与网关端 SoftwareSerial 一致 */

/* ==================== LoRa 模块 AUX 引脚 ====================
 * AUX 是模块输出, 反映模块忙闲状态:
 *   高=数据发送中/接收中/模式切换中(忙)
 *   低=发送完成/接收完成/切换完成(闲)
 * 节点端接 STM32F103 的 PA11 (该脚默认 USART1_CTS / USB_DM, 但本项目
 * 未用 USART1 硬件流控也未用 USB, 故空闲可用作普通 GPIO 输入).
 * 发送前后查 AUX 状态, 判定模块是否收到/发完, 异常时日志报 FAIL.
 * 与网关端 hw_cfg.h 的 LORA_AUX_PIN/LORA_AUX_WAIT_MS 对称配置 */
#define LORA_AUX_PORT       GPIOA
#define LORA_AUX_PIN        GPIO_Pin_11   /* PA11 */
#define LORA_AUX_WAIT_MS    50UL          /* 等 AUX 变化的超时(ms) */

/* ==================== OneNET 子设备证书 ====================
 * 节点通过 LoRa 上报证书给网关, 网关代为上线 OneNET (网关+子设备模式)
 * 子设备证书(产品ID/设备名)默认值宏已迁移至 node_config.h
 * (NODE_PRODUCT_KEY / NODE_DEVICE_NAME) */

/* ==================== 命令回调 ==================== */
/* 网关下行命令回调函数类型
 * cmd:  命令名称(如 "AT+DATA", "AT+CER", "AT+SetLed")
 * value: 命令参数值(如 "0", "1", 无参数时为NULL) */
typedef void (*LoRaCmdCallback)(const char *cmd, const char *value);

/* ==================== 接口函数 ==================== */

/****************************************************************************
 * 初始化 LoRa 模块(定点传输模式)
 * - 初始化 USART2
 * - 配置 LoRa 模块地址和信道
 * - 清空接收缓冲
 ****************************************************************************/
void LoRa_Node_Init(void);

/****************************************************************************
 * 发送证书给网关(首次上线/网关查询时调用)
 * cert: 节点证书结构体指针 (LoraNodeCert_t 见共享头 lora_protocol.h)
 ****************************************************************************/
void LoRa_Node_SendCert(const LoraNodeCert_t *cert);

/****************************************************************************
 * 发送传感器数据给网关(网关查询数据时调用)
 * data: 节点数据结构体指针 (LoraNodeData_t 见共享头 lora_protocol.h)
 ****************************************************************************/
void LoRa_Node_SendData(const LoraNodeData_t *data);

/****************************************************************************
 * 发送命令执行确认给网关
 * cmd: 原始命令字符串
 ****************************************************************************/
void LoRa_Node_SendAck(const char *cmd);

/****************************************************************************
 * 轮询接收网关命令(非阻塞)
 * cb: 命令回调函数, 收到命令时调用
 * 返回值: 1=收到并处理了命令, 0=无命令
 * 说明: 网关通过定点传输发送 AT 命令(如 "AT+DATA\r\n"),
 *       节点解析后回调, 回调中可调用 SendData/SendCert 发送响应
 ****************************************************************************/
uint8_t LoRa_Node_Poll(LoRaCmdCallback cb);

#endif /* __LORA_NODE_H */
