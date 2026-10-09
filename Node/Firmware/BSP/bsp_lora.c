﻿/****************************************************************************
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

/* 单帧最大载荷: 原为 1(帧头)+64(ACK)+2(\r\n)=67; S5 起容纳缩略图帧 0xF2
 * (1类型+5头+200数据+2CRC = 208), 上限取 215 —— 3B定点头+215=218B 仍 ≤ 模块 230B 单包上限 */
#define LORA_TX_PAYLOAD_MAX  215

/* 组帧缓冲: 静态而非栈上 —— 218B 对 1KB 启动栈(0x400)占比太大,
 * 现场栈溢出=硬错误难排查; LoRa 发送只在主循环上下文, 无重入 */
static uint8_t s_txFrame[3 + LORA_TX_PAYLOAD_MAX];

/****************************************************************************
 * 发送定点传输帧
 * 目标地址 + 信道 + 数据
 *
 * ⭐ TX 非阻塞(DMA): 整帧(定点头 + 载荷)先组装到本地缓冲, 再交给
 *   Usart2_SendAsync() 由 DMA1_Channel7 后台搬运, 本函数立即返回;
 *   不再逐字节等 TC 阻塞主循环(原一帧 19~70B 会堵 20~73ms @9600bps).
 *   Usart2_SendAsync() 内部会先把数据拷入静态缓冲, 故此处本地缓冲
 *   可安全出栈.
 *
 * ⭐ AUX 收敛(S23): 只保留"发前等 AUX 低"用于半双工防撞包,
 *   去掉原先"发后等 AUX 高 / 等 AUX 低"的两段忙等(各 50ms 上限).
 *   理由: 发送完成确认属诊断信息, 网关侧本就有超时+重试兜底;
 *         而这两段等待每次都会把主循环(含超声波采样)最多再堵 100ms.
 *   发送是否已完成由下一次发送前的"等 AUX 低"自然衔接(等效流控),
 *   故去掉后不会出现半双工撞包.
 *   发前等待若超时仍为高电平, 说明模块卡在发送/接收/切换, 保留 FAIL 日志.
 * 节点端不接 OLED, 仅打串口日志(OK/FAIL + 耗时)
 ****************************************************************************/
static void LoRa_SendFrame(uint16_t dstAddr, uint8_t ch,
                           const uint8_t *data, uint16_t len)
{
    uint8_t  *frame = s_txFrame;   /* S5: 改静态缓冲(见 s_txFrame 注释) */
    uint8_t  auxBefore;
    uint32_t t0, durBefore = 0;

    /* === 发前: 等 AUX 低(模块空闲), 超时强制发送 === */
    t0 = Get_Tick();
    while (AUX_Read() == 1 &&
           (Get_Tick() - t0) <= LORA_AUX_WAIT_MS) { }
    auxBefore = AUX_Read();   /* 期望 0=空闲 */
    durBefore = Get_Tick() - t0;

    /* === 组装完整帧: 定点头(AddrH/AddrL/CH) + 载荷 === */
    frame[0] = (uint8_t)(dstAddr >> 8);    /* 地址高字节 */
    frame[1] = (uint8_t)(dstAddr & 0xFF);  /* 地址低字节 */
    frame[2] = ch;                         /* 信道 */
    if (len > LORA_TX_PAYLOAD_MAX)
        len = LORA_TX_PAYLOAD_MAX;         /* 防御性截断(协议帧不会到这) */
    if (len > 0)
        memcpy(frame + 3, data, len);

    /* === 非阻塞发送: DMA 后台搬运, 立即返回 === */
    Usart2_SendAsync(frame, (uint16_t)(3 + len));

    /* === 日志: 发前状态 + 等待耗时 ===
     * 正常路径只一行 OK; 发前 AUX 一直高则一行 FAIL */
    if (auxBefore == 0)
    {
        Usart_Printf(USART_DEBUG,
                     "[LoRa] TX 0x%04X OK (发前AUX闲 %lums)\r\n",
                     (unsigned)dstAddr, (unsigned long)durBefore);
    }
    else
    {
        Usart_Printf(USART_DEBUG,
                     "[LoRa] TX 0x%04X FAIL 发前AUX忙%lums(模块卡在发送/接收/切换)\r\n",
                     (unsigned)dstAddr, (unsigned long)durBefore);
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

void LoRa_Node_SendPlate(const LoraPlate_t *p)
{
    uint8_t buf[1 + sizeof(LoraPlate_t)];
    LoraPlate_t tmp;

    /* v4 协议: 车牌为事件量, 独立成帧(0xF1), 发送前算 CRC; 无 seq 字段 */
    memcpy(&tmp, p, sizeof(tmp));
    tmp.crc16 = lora_crc16((const uint8_t*)&tmp, offsetof(LoraPlate_t, crc16));

    /* 帧格式: 帧头 + [帧头字节][车牌结构体], 一次性发送(与 SendData 相同) */
    buf[0] = LORA_FRAME_PLATE;
    memcpy(buf + 1, &tmp, sizeof(LoraPlate_t));
    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, sizeof(buf));

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 车牌: %s conf=%d valid=%d src=%d frameNo=%lu crc=%04X\r\n",
                 tmp.plate, tmp.conf, tmp.valid, tmp.source,
                 (unsigned long)tmp.frameNo, tmp.crc16);
}

/****************************************************************************
 * 发送车牌缩略图(S5, 响应网关 AT+IMG): 282B 拆 2 包
 *   第1包 dataLen=200 立即发; 第2包 82B 由 ImgTx_Poll() 在第1包**空中发完**
 *   (AUX 转闲, 且距第1包 >=300ms)后再发 —— 9600bps 下第1包光串口排空就要
 *   ~220ms, 若立即发第2包, 其 50ms AUX 等待会超时强制发送, 半双工直接撞坏第1包。
 * 两包都发满整帧(sizeof(LoraImgFrame_t)+1=208B, 未用数据补 0, dataLen 指示真实长度):
 *   网关接收状态机按固定长度收帧(与 0xF1 同范式), 不必按 dataLen 动态改收长。
 * 线格式: [0xF2][imgNo 2][total][idx][dataLen][data 200][crc16 2]
 *   crc16 覆盖结构体首至 offsetof(crc16) 前(buf[1..205]), 与共享头定义一致。
 * 字段用 memcpy 落位: 结构体在缓冲区偏移 1, 不能按对齐指针直写。
 ****************************************************************************/
static struct {
    uint8_t        left;    /* 1=第2包待发 */
    uint16_t       imgNo;
    const uint8_t *img2;    /* 第2包数据(指向调用方全图缓存+200, 调用方保证存活) */
    uint32_t       at;      /* 第1包发出时刻 */
} s_imgTx;

static void Img_SendFrag(uint16_t imgNo, uint8_t idx,
                         const uint8_t *data, uint8_t dataLen)
{
    static uint8_t buf[1 + sizeof(LoraImgFrame_t)];   /* 208B, 静态省栈(同 s_txFrame 理由) */
    uint16_t crc;

    if (dataLen > LORA_IMG_FRAG_MAX)
        dataLen = LORA_IMG_FRAG_MAX;

    memset(buf, 0, sizeof(buf));
    buf[0] = LORA_FRAME_IMG;
    memcpy(buf + 1, &imgNo, 2);
    buf[3] = 2;                /* total: 本图固定 2 包 */
    buf[4] = idx;
    buf[5] = dataLen;
    if (dataLen > 0)
        memcpy(buf + 6, data, dataLen);
    crc = lora_crc16(buf + 1, offsetof(LoraImgFrame_t, crc16));
    memcpy(buf + 1 + offsetof(LoraImgFrame_t, crc16), &crc, 2);

    LoRa_SendFrame(LORA_GATEWAY_ADDR, LORA_CHANNEL, buf, (uint16_t)sizeof(buf));

    Usart_Printf(USART_DEBUG, "[LoRa] 发送-> 缩略图: imgNo=%u 包%u/2 数据%uB crc=%04X\r\n",
                 (unsigned)imgNo, (unsigned)idx, (unsigned)dataLen, crc);
}

void LoRa_Node_SendImg(uint16_t imgNo, const uint8_t *img)
{
    Img_SendFrag(imgNo, 1, img, LORA_IMG_FRAG_MAX);
    s_imgTx.imgNo = imgNo;
    s_imgTx.img2  = img + LORA_IMG_FRAG_MAX;
    s_imgTx.at    = Get_Tick();
    s_imgTx.left  = 1;
}

/* 第2包择机发送: 由 LoRa_Node_Poll 每轮调(主循环节拍) */
static void ImgTx_Poll(void)
{
    if (!s_imgTx.left)
        return;
    if ((uint32_t)(Get_Tick() - s_imgTx.at) < 300UL)
        return;                                   /* 等第1包串口排空 + 起呼 */
    if (AUX_Read() == 1)
    {                                             /* 第1包还在空中 */
        if ((uint32_t)(Get_Tick() - s_imgTx.at) > 5000UL)
        {
            s_imgTx.left = 0;
            Usart_Printf(USART_DEBUG, "[LoRa][ERR] 缩略图第2包放弃(第1包5s未发完)\r\n");
        }
        return;
    }
    Img_SendFrag(s_imgTx.imgNo, 2, s_imgTx.img2,
                 (uint8_t)(LORA_IMG_BYTES - LORA_IMG_FRAG_MAX));
    s_imgTx.left = 0;
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

    /* S5: 缩略图第2包择机补发(第1包空中发完后, 见 ImgTx_Poll) */
    ImgTx_Poll();

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
