﻿﻿/****************************************************************************
 * LoRa 节点通信驱动 - bsp_lora.c
 *
 * 功能描述:
 *   实现节点端 LoRa 定点传输通信驱动
 *   使用 USART2 与 LoRa 模块通信, 支持定点传输模式
 *
 * 定点传输帧格式:
 *   [AddrH][AddrL][CH] + 数据载荷
 *   目标时: AddrH/AddrL=目标地址, CH=信道
 *   源端时: 模块自动填充源地址, 无需手动指定
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-07
 */

#include <string.h>
#include <stdlib.h>
#include "bsp_lora.h"
#include "bsp_usart.h"
#include "bsp_delay.h"

/* ==================== 内部变量 ==================== */

static uint8_t  s_online = 0;           /* 在线状态 */
static char     s_rxBuf[128];           /* 接收缓冲区 */
static uint16_t s_rxLen = 0;            /* 接收数据长度 */

/* ==================== 内部函数 ==================== */

/****************************************************************************
 * 发送定点传输帧
 * 目标地址 + 信道 + 数据
 ****************************************************************************/
static void LoRa_SendFrame(uint16_t dstAddr, uint8_t ch,
                           const uint8_t *data, uint16_t len)
{
    uint8_t header[3];
    header[0] = (uint8_t)(dstAddr >> 8);    /* 地址高字节 */
    header[1] = (uint8_t)(dstAddr & 0xFF);  /* 地址低字节 */
    header[2] = ch;                         /* 信道 */

    /* 发送帧头 */
    Usart_SendString(USART2, header, 3);
    /* 发送数据载荷 */
    Usart_SendString(USART2, (unsigned char *)data, len);
}

/****************************************************************************
 * 从环形缓冲区读取接收数据
 * 返回值: 实际读取的字节数
 * 说明:   通过 bsp_usart.c 的 Usart2_GetData() 从 USART2 环形缓冲区取数,
 *         环形缓冲区在 USART_FLAG_RXNE 中断中逐字节写入
 ****************************************************************************/
static uint16_t LoRa_ReadRx(void)
{
    uint16_t avail = sizeof(s_rxBuf) - 1 - s_rxLen;
    uint16_t count = 0;

    if (avail == 0)
        return 0;

    count = Usart2_GetData((uint8_t *)(s_rxBuf + s_rxLen), avail);
    s_rxLen += count;

    return count;
}

/****************************************************************************
 * 检查接收缓冲区是否有完整命令(以 \r\n 结尾)
 * 返回值: 1=有完整命令, 0=没有
 ****************************************************************************/
static uint8_t LoRa_HasCompleteCmd(void)
{
    uint16_t i;
    for (i = 0; i < s_rxLen; i++)
    {
        if (s_rxBuf[i] == '\n' && i > 0 && s_rxBuf[i - 1] == '\r')
            return 1;
    }
    return 0;
}

/****************************************************************************
 * 解析并执行一个命令
 * 提取命令名称和参数, 调用回调函数
 * 返回值: 1=成功解析, 0=解析失败(无完整命令)
 ****************************************************************************/
static uint8_t LoRa_ParseCmd(LoRaCmdCallback cb)
{
    uint16_t i;
    char cmd[32];
    char value[32];
    char *pEq;
    uint16_t cmdLen;

    /* 查找 \r\n 分隔符 */
    for (i = 0; i < s_rxLen; i++)
    {
        if (s_rxBuf[i] == '\r' && i + 1 < s_rxLen && s_rxBuf[i + 1] == '\n')
            break;
    }
    if (i >= s_rxLen)
        return 0;

    cmdLen = i;
    if (cmdLen >= sizeof(cmd))
        cmdLen = sizeof(cmd) - 1;
    memcpy(cmd, s_rxBuf, cmdLen);
    cmd[cmdLen] = '\0';

    /* 移除已处理的数据(含\r\n) */
    {
        uint16_t consumed = cmdLen + 2;
        memmove(s_rxBuf, s_rxBuf + consumed, s_rxLen - consumed);
        s_rxLen -= consumed;
    }

    /* 解析命令参数: AT+XXX=value */
    pEq = strchr(cmd, '=');
    if (pEq)
    {
        uint16_t vlen;
        *pEq = '\0';
        vlen = strlen(pEq + 1);
        if (vlen >= sizeof(value))
            vlen = sizeof(value) - 1;
        memcpy(value, pEq + 1, vlen);
        value[vlen] = '\0';
        cb(cmd, value);
    }
    else
    {
        cb(cmd, NULL);
    }

    return 1;
}

/* ==================== 接口函数实现 ==================== */

void LoRa_Node_Init(void)
{
    /* 初始化 USART2, 配置 LoRa 模块 */
    Usart2_Init(LORA_BAUD);

    /* TODO: 配置 LoRa 模块为定点传输模式
     * 后续需要增加对 LoRa 模块的初始化命令, 如:
     *   AT+ADDRESS=0x0001     设置节点地址
     *   AT+NETWORKID=0x0000   设置网络ID
     *   AT+MODE=1             设置定点传输模式
     *   AT+BAUD=115200        设置波特率
     *   AT+PARAMETER=10,7,1,12 设置SF/BW/CR/Preamble
     *
     * 当前 LoRa 模块通过串口透传方式使用, 默认为定点传输模式
     * 无需额外 AT 指令配置 */

    s_online = 0;
    s_rxLen = 0;

    Usart_Printf(USART_DEBUG, "[LoRa] 初始化: 地址=0x%04X 信道=%d 波特率=%d\r\n",
                 LORA_NODE_ADDR, LORA_CHANNEL, LORA_BAUD);
}

void LoRa_Node_SendCert(const NodeCert_t *cert)
{
    uint8_t buf[1 + sizeof(NodeCert_t)];

    /* 帧格式: 帧头 + [帧头字节][证书结构体]
     * 注意: 通过一次 LoRa_SendFrame 发送, 帧头/数据由底层处理,
     *       模块自动添加目标地址帧, 无需手动填充 */
    buf[0] = LORA_FRAME_CERT;
    memcpy(buf + 1, cert, sizeof(NodeCert_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 证书: 产品=%s 设备=%s valid=%d\r\n",
                 cert->ProductKey, cert->DeviceName, cert->valid);
}

void LoRa_Node_SendData(const NodeData_t *data)
{
    uint8_t buf[1 + sizeof(NodeData_t)];

    /* 帧格式: 帧头 + [帧头字节][数据结构体], 一次性发送(与 SendCert 相同) */
    buf[0] = LORA_FRAME_DATA;
    memcpy(buf + 1, data, sizeof(NodeData_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    s_online = 1;   /* 标记为已上线 */

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 数据: 位置=%d 距离=%dcm 地磁=%d 时长=%lus LED=%d 使能=%d\r\n",
                 data->ParkStatus, data->Ultrasonic, data->GeoMagnetic,
                 (unsigned long)data->OccupiedTime, data->LED, data->LedEnable);
}

void LoRa_Node_SendAck(const char *cmd)
{
    uint8_t buf[1 + 64 + 2];
    uint16_t cmdLen = strlen(cmd);

    /* 帧格式: 帧头 + [帧头字节][原命令字符串], 一次性发送(与 SendCert 相同)
     * 注意: ACK 字符串必须包含 \r\n 结束符, 网关端以 \r 分割;
     *       去除 \r\n 后可能导致 ACK 超出单帧(网关端限制),
     *       但 PONG 等短命令完全能满足长度要求 */
    if (cmdLen > 64)
        cmdLen = 64;
    buf[0] = LORA_FRAME_ACK;
    memcpy(buf + 1, cmd, cmdLen);
    buf[1 + cmdLen]     = '\r';
    buf[1 + cmdLen + 1] = '\n';
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, 1 + cmdLen + 2);

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 确认: %s\r\n", cmd);
}

uint8_t LoRa_Node_Poll(LoRaCmdCallback cb)
{
    uint8_t processed = 0;

    /* 读取所有可用数据 */
    LoRa_ReadRx();

    /* 循环处理所有完整命令 */
    while (LoRa_HasCompleteCmd())
    {
        if (LoRa_ParseCmd(cb))
            processed = 1;
    }

    return processed;
}

uint8_t LoRa_Node_IsOnline(void)
{
    return s_online;
}
