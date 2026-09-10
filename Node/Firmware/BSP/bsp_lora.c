/****************************************************************************
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
#include "node_config.h"   /* 节点身份运行时变量(产品ID/设备名) */
#include "bsp_usart.h"
#include "bsp_delay.h"

/* ==================== 内部变量 ==================== */
/* ⭐ S22: 节点侧不维护独立在线位(s_online 已移除),
 * 在线状态以网关轮询交互为准(网关侧三态模型 S9/S12/S13) */

static char     s_rxBuf[128];           /* 接收缓冲区 */
static uint16_t s_rxLen = 0;            /* 接收数据长度 */
static uint8_t  loraSeq  = 0;           /* v2 协议帧序列号, 每次发送 ++, 0..255 循环 */

/* ==================== 内部函数 ==================== */

/* 读取 AUX 引脚电平, 返回 1=高(忙), 0=低(闲) */
static uint8_t AUX_Read(void)
{
    return (GPIO_ReadInputDataBit(LORA_AUX_PORT, LORA_AUX_PIN) == Bit_SET) ? 1 : 0;
}

/****************************************************************************
 * 发送定点传输帧
 * 目标地址 + 信道 + 数据
 *
 * AUX 忙闲状态判定 (状态码轨迹 [auxBefore->sawHigh->auxAfter]):
 *   [0->1->0] 正常: 发前空闲 -> 发后捕到高(模块收到) -> 等回低(发送完成)
 *   [1->?->?] 发前 AUX 一直高, 模块卡在发送/接收/切换
 *   [0->0->0] 发后 AUX 没变高, 模块未收到数据 (串口/接线异常)
 *   [0->1->1] 发后 AUX 一直高, 模块卡死在发送中
 * 节点端不接 OLED, 仅打串口日志(OK/FAIL + 轨迹码 + 耗时)
 ****************************************************************************/
static void LoRa_SendFrame(uint16_t dstAddr, uint8_t ch,
                           const uint8_t *data, uint16_t len)
{
    uint8_t header[3];
    uint8_t auxBefore, sawHigh, auxAfter;
    uint32_t t0, t1, t2, durBefore = 0, durHigh = 0, durLow = 0;

    /* === 发前: 等 AUX 低(模块空闲), 超时强制发送 === */
    t0 = Get_Tick();
    while (AUX_Read() == 1 &&
           (Get_Tick() - t0) <= LORA_AUX_WAIT_MS) { }
    auxBefore = AUX_Read();   /* 期望 0=空闲 */
    durBefore = Get_Tick() - t0;

    /* === 发数据 === */
    header[0] = (uint8_t)(dstAddr >> 8);    /* 地址高字节 */
    header[1] = (uint8_t)(dstAddr & 0xFF);  /* 地址低字节 */
    header[2] = ch;                         /* 信道 */

    /* 发送帧头 */
    Usart_SendString(USART2, header, 3);
    /* 发送数据载荷 */
    if (len > 0) Usart_SendString(USART2, (unsigned char *)data, len);

    /* === 发后: 等 AUX 高(模块收到开始处理) -> 等 AUX 低(发送完成) === */
    t1 = Get_Tick();
    while (AUX_Read() == 0 &&
           (Get_Tick() - t1) <= LORA_AUX_WAIT_MS) { }   /* 等高 */
    sawHigh = AUX_Read();   /* 期望 1=已变高 */
    durHigh = Get_Tick() - t1;

    t2 = Get_Tick();
    while (AUX_Read() == 1 &&
           (Get_Tick() - t2) <= LORA_AUX_WAIT_MS) { }   /* 等低 */
    auxAfter = AUX_Read();   /* 期望 0=完成 */
    durLow = Get_Tick() - t2;

    /* === 日志: 状态码轨迹 + OK/FAIL ===
     * 正常路径只一行 OK, 带目标地址 + 轨迹码 + 总耗时
     * 异常路径一行 FAIL, 带目标地址 + 轨迹码 + 原因 + 各阶段耗时 */
    if (auxBefore == 0 && sawHigh == 1 && auxAfter == 0)
    {
        Usart_Printf(USART_DEBUG,
                     "[LoRa] TX 0x%04X OK [%d->%d->%d] %lums\r\n",
                     (unsigned)dstAddr,
                     (unsigned)auxBefore, (unsigned)sawHigh, (unsigned)auxAfter,
                     (unsigned long)(durBefore + durHigh + durLow));
    }
    else
    {
        const char *reason;
        if (auxBefore == 1)
            reason = "发前AUX忙, 模块卡在发送/接收/切换";
        else if (sawHigh == 0)
            reason = "模块未收到数据, 串口/接线异常";
        else if (auxAfter == 1)
            reason = "发后AUX一直高, 模块卡死在发送中";
        else
            reason = "未知异常";
        Usart_Printf(USART_DEBUG,
                     "[LoRa] TX 0x%04X FAIL [%d->%d->%d] %s (前%lu/等高%lu/等低%lu ms)\r\n",
                     (unsigned)dstAddr,
                     (unsigned)auxBefore, (unsigned)sawHigh, (unsigned)auxAfter,
                     reason,
                     (unsigned long)durBefore,
                     (unsigned long)durHigh,
                     (unsigned long)durLow);
    }
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
 * ⭐ 缓冲区死锁兜底: s_rxBuf 快满却找不到完整命令(\r\n)时,
 * 说明当前全是无效/残缺数据(LoRa 模块噪声/帧头残留/丢字节致命令断裂),
 * 必须整体丢弃, 否则 s_rxLen 永远卡在 127, 之后 LoRa_ReadRx 不再取数,
 * 环形缓冲区随之堆满, 新命令全被丢弃 -> 节点永久失去 LoRa 命令处理能力,
 * 但主循环照跑/喂狗照打 -> 表现为"正常通信中突然掉线且不再恢复".
 *
 * 触发条件: s_rxLen >= 阈值(留 8 字节余量) 且 整段缓冲区没有 \r\n
 * 处理:   清空 s_rxBuf, s_rxLen = 0, 打印一次警告便于追溯
 ****************************************************************************/
static void LoRa_FlushStaleIfFull(void)
{
    uint16_t i;

    if (s_rxLen < (uint16_t)(sizeof(s_rxBuf) - 8))
        return;   /* 还没接近满, 不用处理 */

    /* 整段扫描有没有 \r\n */
    for (i = 0; i < s_rxLen; i++)
    {
        if (s_rxBuf[i] == '\n' && i > 0 && s_rxBuf[i - 1] == '\r')
            return;   /* 至少有一条完整命令, 让正常流程去消费 */
    }

    /* 没有任何 \r\n 却快满了 -> 全是残缺/无效数据, 丢弃避免死锁 */
    Usart_Printf(USART_DEBUG,
                 "[LoRa][WARN] 丢弃 %d 字节无效数据(无\\r\\n, 防缓冲区死锁)\r\n",
                 (int)s_rxLen);
    s_rxLen = 0;
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

    /* ⭐ 过滤模块发送回执的空行(\r\n): cmdLen==0 说明收到的只有换行,
     * 是 LoRa 模块发送完成后输出的状态回执, 非网关命令, 跳过不触发回调 */
    if (cmdLen == 0)
        return 1;

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
    GPIO_InitTypeDef gpio;

    /* 初始化 USART2, 配置 LoRa 模块 */
    Usart2_Init(LORA_BAUD);

    /* 配置 AUX 引脚 (PA11) 为浮空输入.
     * AUX 是 LoRa 模块输出, 反映忙闲状态(高=忙, 低=闲);
     * STM32 端读取电平, 在发送前后判断模块是否就绪.
     * 选浮空输入而非上拉: 模块侧已有明确输出驱动, 无需内部上下拉 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = LORA_AUX_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;   /* 输入模式下 Speed 无意义, 但字段需赋值 */
    GPIO_Init(LORA_AUX_PORT, &gpio);

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

    s_rxLen = 0;

    Usart_Printf(USART_DEBUG, "[LoRa] 初始化: 信道=%d 波特率=%d (AUX=PA11)\r\n",
                 LORA_CHANNEL, LORA_BAUD);
}

void LoRa_Node_SendCert(const LoraNodeCert_t *cert)
{
    uint8_t buf[1 + sizeof(LoraNodeCert_t)];
    LoraNodeCert_t tmp;

    /* v2 协议: 发送前填 seq + 算 CRC, 防止链路错位/噪声被网关当合法帧解析 */
    memcpy(&tmp, cert, sizeof(tmp));
    tmp.seq = ++loraSeq;
    tmp.crc16 = lora_crc16((const uint8_t*)&tmp, offsetof(LoraNodeCert_t, crc16));

    /* 帧格式: 帧头 + [帧头字节][证书结构体]
     * 注意: 通过一次 LoRa_SendFrame 发送, 帧头/数据由底层处理,
     *       模块自动添加目标地址帧, 无需手动填充 */
    buf[0] = LORA_FRAME_CERT;
    memcpy(buf + 1, &tmp, sizeof(LoraNodeCert_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 证书: 产品=%s 设备=%s valid=%d seq=%d crc=%04X\r\n",
                 cert->ProductKey, cert->DeviceName, cert->valid, tmp.seq, tmp.crc16);
}

void LoRa_Node_SendData(const LoraNodeData_t *data)
{
    uint8_t buf[1 + sizeof(LoraNodeData_t)];
    LoraNodeData_t tmp;

    /* v2 协议: 发送前填 seq + 算 CRC, 网关端 CRC 不通过的帧直接丢弃 */
    memcpy(&tmp, data, sizeof(tmp));
    tmp.seq = ++loraSeq;
    tmp.crc16 = lora_crc16((const uint8_t*)&tmp, offsetof(LoraNodeData_t, crc16));

    /* 帧格式: 帧头 + [帧头字节][数据结构体], 一次性发送(与 SendCert 相同) */
    buf[0] = LORA_FRAME_DATA;
    memcpy(buf + 1, &tmp, sizeof(LoraNodeData_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 数据: 位置=%d 距离=%dcm 地磁=%d 时长=%lus LED=%d seq=%d crc=%04X\r\n",
                 data->ParkStatus, data->Ultrasonic, data->GeoMagnetic,
                 (unsigned long)data->OccupiedTime, data->LED,
                 tmp.seq, tmp.crc16);
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

    /* ⭐ 死锁兜底: 命令处理完后, 如果缓冲区快满仍无完整命令,
     * 说明全是无效数据, 清空防止永久卡死(根因修复) */
    LoRa_FlushStaleIfFull();

    return processed;
}
