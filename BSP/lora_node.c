/****************************************************************************
 * LoRa 节点通信驱动 - lora_node.c
 *
 * 功能描述:
 *   升级现有 LoRa 透传模块为定点传输驱动
 *   使用 USART2 与 LoRa 模块通信, 定点传输模式
 *
 * 定点传输帧格式:
 *   [AddrH][AddrL][CH] + 数据内容
 *   发送时: AddrH/AddrL=目标地址, CH=信道
 *   接收时: 模块自动过滤非本地址数据, 输出数据内容
 *
 * 作者: Bjmyhc
 * 日期: 2026-08-07
 ****************************************************************************/

#include <string.h>
#include <stdlib.h>
#include "lora_node.h"
#include "bsp_usart.h"
#include "bsp_delay.h"

/* ==================== 内部变量 ==================== */

static uint8_t  s_online = 0;           /* 在线状态 */
static char     s_rxBuf[128];           /* 接收缓冲 */
static uint16_t s_rxLen = 0;            /* 接收长度 */
static uint32_t s_lastRxTick = 0;       /* 上次接收时间 */

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
    /* 发送数据 */
    Usart_SendString(USART2, (unsigned char *)data, len);
}

/****************************************************************************
 * 非阻塞读取串口数据到接收缓冲
 * 返回值: 本次读取的字节数
 * 说明:   通过 bsp_usart.c 的 Usart2_GetData() 从 USART2 中断环形缓冲读取,
 *         避免轮询 USART_FLAG_RXNE 时被其他中断打断丢字节
 ****************************************************************************/
static uint16_t LoRa_ReadRx(void)
{
    uint16_t avail = sizeof(s_rxBuf) - 1 - s_rxLen;
    uint16_t count = 0;

    if (avail == 0)
        return 0;

    count = Usart2_GetData((uint8_t *)(s_rxBuf + s_rxLen), avail);
    s_rxLen += count;

    if (count > 0)
        s_lastRxTick = Get_Tick();

    return count;
}

/****************************************************************************
 * 检查接收缓冲是否包含完整命令(以 \r\n 结尾)
 * 返回值: 1=有完整命令, 0=不完整
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
 * 解析并执行一条命令
 * 提取命令名和参数, 调用回调函数
 * 返回值: 1=成功解析, 0=无完整命令
 ****************************************************************************/
static uint8_t LoRa_ParseCmd(LoRaCmdCallback cb)
{
    uint16_t i;
    char cmd[32];
    char value[32];
    char *pEq;
    uint16_t cmdLen;

    /* 找 \r\n */
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

    /* 移除已处理的命令(含\r\n) */
    {
        uint16_t consumed = cmdLen + 2;
        memmove(s_rxBuf, s_rxBuf + consumed, s_rxLen - consumed);
        s_rxLen -= consumed;
    }

    /* 解析命令名和参数: AT+XXX=value */
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
    /* 初始化 USART2, 连接 LoRa 模块 */
    Usart2_Init(LORA_BAUD);

    /* TODO: 配置 LoRa 模块为定点传输模式
     * 具体命令取决于 LoRa 模块型号, 例如:
     *   AT+ADDRESS=0x0001     设置节点地址
     *   AT+NETWORKID=0x0000   设置网络ID
     *   AT+MODE=1             设置定点传输模式
     *   AT+BAUD=115200        设置波特率
     *   AT+PARAMETER=10,7,1,12  设置SF/BW/CR/Preamble
     *
     * 当前假设 LoRa 模块已通过出厂配置或手动配置为定点传输模式
     * 后续可添加 AT 指令配置代码 */

    s_online = 0;
    s_rxLen = 0;

    Usart_Printf(USART_DEBUG, "LoRa Node Init OK (addr=0x%04X, ch=%d)\r\n",
                 LORA_NODE_ADDR, LORA_CHANNEL);
}

void LoRa_Node_SendCert(const NodeCert_t *cert)
{
    uint8_t buf[1 + sizeof(NodeCert_t)];

    /* 帧格式: 地址头 + [帧头字节][证书结构体]
     * 注意: 必须一次 LoRa_SendFrame 发完, 若帧头/数据分两次发送,
     *       模块会把第二段的地址头字节混入数据, 导致接收端错位 */
    buf[0] = LORA_FRAME_CERT;
    memcpy(buf + 1, cert, sizeof(NodeCert_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    Usart_Printf(USART_DEBUG, "LoRa: Send cert\r\n");
}

void LoRa_Node_SendData(const NodeData_t *data)
{
    uint8_t buf[1 + sizeof(NodeData_t)];

    /* 帧格式: 地址头 + [帧头字节][数据结构体], 一次发送(见 SendCert 注释) */
    buf[0] = LORA_FRAME_DATA;
    memcpy(buf + 1, data, sizeof(NodeData_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    s_online = 1;   /* 标记在线 */

    Usart_Printf(USART_DEBUG, "LoRa: Send data (park=%d, us=%d, mag=%d)\r\n",
                 data->ParkStatus, data->Ultrasonic, data->GeoMagnetic);
}

void LoRa_Node_SendAck(const char *cmd)
{
    uint8_t buf[1 + 64];
    uint16_t cmdLen = strlen(cmd);

    /* 帧格式: 地址头 + [帧头字节][命令字符串], 一次发送(见 SendCert 注释) */
    if (cmdLen > 64)
        cmdLen = 64;
    buf[0] = LORA_FRAME_ACK;
    memcpy(buf + 1, cmd, cmdLen);
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, 1 + cmdLen);

    Usart_Printf(USART_DEBUG, "LoRa: Send ACK (%s)\r\n", cmd);
}

uint8_t LoRa_Node_Poll(LoRaCmdCallback cb)
{
    uint8_t processed = 0;

    /* 读取串口数据 */
    LoRa_ReadRx();

    /* 解析并执行所有完整命令 */
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
