/* lora_handler.h - Gateway LoRa 轮询调度 + 定点传输 */
#ifndef LORA_HANDLER_H
#define LORA_HANDLER_H

#include <Arduino.h>
#include "lora_protocol.h"

/* 初始化 LoRa 串口(硬件或软串). 会配置 AUX 输入方向,
 * 等 AUX 拉低(模块初始化完成/空闲)后才开串口,
 * 避免上电首帧被模块内部初始化吞掉.
 * M0/M1 硬件直连 GND (强制 M0=M1=0 高时效模式), 不再占用 GPIO. */
void lora_init(void);

/* 主循环周期调用:
 *   - 从串口感知识别帧头字节 -> 解析二进制结构体 -> 更新 node_data
 *   - 调度下一轮询: 未注册节点发 AT+CERx, 已注册节点发 AT+DATAx
 *   - 超时不回复则跳过, 下一轮再查
 * 返回值: 本轮是否有新的节点数据到达 (dataChanged 可能被置位) */
bool lora_tick(void);

/* 向 nodeId 节点下发设置命令
 * property: 命令属性 (如 "LedEnable")
 * value: 0 或 1 (整数)
 * 说明: 网关在当前轮询周期结束后插入, 下一条发 AT+<property><nodeId>=<value> */
void lora_sendControl(uint8_t nodeId, const char *property, int value);

/* 手动触发节点发现: 从头扫描所有节点 */
void lora_triggerDiscovery(void);

/* 查询是否正处于节点发现扫描中 (discoveryMode 置位期间返回 true,
 * 供 OLED 扫描画面判断"扫描是否真实结束"以决定退出时机) */
bool lora_discoveryActive(void);

/* 获取 LoRa 串口引用(调试) */
Stream &lora_getSerial(void);

#endif /* LORA_HANDLER_H */
