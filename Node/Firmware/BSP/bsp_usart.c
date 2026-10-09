/****************************************************************************
 * 串口驱动 - bsp_usart.c
 * 
 * 功能描述:
 *   实现USART1/USART2/USART3的初始化配置、数据发送、格式化打印等功能
 *   USART1用于调试输出，USART2用于与 LoRa 模块通信, USART3 用于与摄像头模组通信
 * 
 * 硬件配置:
 *   - USART1: TX-PA9,  RX-PA10, 波特率115200(调试)
 *   - USART2: TX-PA2,  RX-PA3,  波特率由 LORA_BAUD 决定(9600)
 *   - USART3: TX-PB10, RX-PB11, 波特率115200(摄像头模组)
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "bsp_usart.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "bsp_delay.h"   /* ⭐ 引入 Get_Tick() 用于串口发送超时判断 */
#include "stm32f10x_dma.h"   /* ⭐ USART2/USART3 TX 非阻塞发送使用 DMA1 */

/* ==================== 串口发送超时(ms): 硬件异常时最多阻塞 50ms, 避免死等卡死主循环 ==================== */
#define USART_SEND_TIMEOUT_MS 50

/* ==================== USART2 环形缓冲区变量 ==================== */
static volatile uint8_t  usart2_rbuf[USART2_RBUF_SIZE];
static volatile uint16_t usart2_rhead = 0;    /* 写入指针 */
static volatile uint16_t usart2_rtail = 0;    /* 读取指针 */

/* ==================== USART2 发送缓冲 + DMA(TX 非阻塞) ====================
 * ⭐ DMA 搬运期间缓冲必须保持有效, 故用静态区(不能是调用者的栈变量).
 * 单帧最大长度: 3(定点头 AddrH/AddrL/CH) + 1(帧头) + 64(ACK 命令上限) + 2(\r\n) = 70,
 * 取 128 留足余量 */
#define USART2_TXBUF_SIZE       128
#define USART2_TX_DMA_CHANNEL   DMA1_Channel7     /* USART2_TX = DMA1_Channel7 */
#define USART2_TX_DMA_FLAG_TC   DMA1_FLAG_TC7
#define USART2_TX_DMA_FLAG_GL   DMA1_FLAG_GL7
#define USART2_TX_DMA_WAIT_MS   100UL             /* 等上一帧搬完的上限(正常 0) */

static uint8_t s_usart2_txbuf[USART2_TXBUF_SIZE];

/* ==================== USART3 环形缓冲区 + 发送缓冲 + DMA(TX 非阻塞) ====================
 * 与 USART2 同范式, 但用 DMA1_Channel2(USART3_TX).
 * 摄像头交互命令均为短报文(<=32B), 缓冲取 128 足够 */
static volatile uint8_t  usart3_rbuf[USART3_RBUF_SIZE];
static volatile uint16_t usart3_rhead = 0;    /* 写入指针 */
static volatile uint16_t usart3_rtail = 0;    /* 读取指针 */

#define USART3_TXBUF_SIZE       128
#define USART3_TX_DMA_CHANNEL   DMA1_Channel2     /* USART3_TX = DMA1_Channel2 */
#define USART3_TX_DMA_FLAG_TC   DMA1_FLAG_TC2
#define USART3_TX_DMA_FLAG_GL   DMA1_FLAG_GL2
#define USART3_TX_DMA_WAIT_MS   100UL             /* 等上一帧搬完的上限(正常 0) */

static uint8_t s_usart3_txbuf[USART3_TXBUF_SIZE];

/****************************************************************************
 * 函数名: Usart2_DmaTxInit
 * 功能:   配置 USART2 TX 的 DMA1_Channel7(仅初始化一次固定字段)
 * 参数:   无
 * 返回值: 无
 * 说明:   每次发送只需改 CNDTR 并重新使能通道, 无需重新 DMA_Init
 ****************************************************************************/
static void Usart2_DmaTxInit(void)
{
    DMA_InitTypeDef dma;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(USART2_TX_DMA_CHANNEL);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DR;
    dma.DMA_MemoryBaseAddr     = (uint32_t)s_usart2_txbuf;
    dma.DMA_DIR                = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize         = 0;                       /* 每次发送时再装填 */
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode               = DMA_Mode_Normal;         /* 传完自动关通道 */
    dma.DMA_Priority           = DMA_Priority_High;
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(USART2_TX_DMA_CHANNEL, &dma);

    DMA_Cmd(USART2_TX_DMA_CHANNEL, DISABLE);

    /* 允许 USART2 的 TX 请求(每次 TXE)触发 DMA 搬运 */
    USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);
}

/****************************************************************************
 * 函数名: Usart3_DmaTxInit
 * 功能:   配置 USART3 TX 的 DMA1_Channel2(仅初始化一次固定字段)
 * 参数:   无
 * 返回值: 无
 ****************************************************************************/
static void Usart3_DmaTxInit(void)
{
    DMA_InitTypeDef dma;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(USART3_TX_DMA_CHANNEL);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART3->DR;
    dma.DMA_MemoryBaseAddr     = (uint32_t)s_usart3_txbuf;
    dma.DMA_DIR                = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize         = 0;                       /* 每次发送时再装填 */
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode               = DMA_Mode_Normal;         /* 传完自动关通道 */
    dma.DMA_Priority           = DMA_Priority_Medium;     /* 低于 LoRa 的 High */
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(USART3_TX_DMA_CHANNEL, &dma);

    DMA_Cmd(USART3_TX_DMA_CHANNEL, DISABLE);

    /* 允许 USART3 的 TX 请求(每次 TXE)触发 DMA 搬运 */
    USART_DMACmd(USART3, USART_DMAReq_Tx, ENABLE);
}

/****************************************************************************
 * 函数名: Usart1_Init
 * 功能:   初始化串口1
 * 参数:   baud - 波特率
 * 返回值: 无
 * 引脚:   TX-PA9, RX-PA10
 ****************************************************************************/
void Usart1_Init(unsigned int baud)
{
    GPIO_InitTypeDef gpioInitStruct;
    USART_InitTypeDef usartInitStruct;
    NVIC_InitTypeDef nvicInitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    gpioInitStruct.GPIO_Mode = GPIO_Mode_AF_PP;
    gpioInitStruct.GPIO_Pin = GPIO_Pin_9;
    gpioInitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpioInitStruct);

    gpioInitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpioInitStruct.GPIO_Pin = GPIO_Pin_10;
    gpioInitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpioInitStruct);

    usartInitStruct.USART_BaudRate = baud;
    usartInitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usartInitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usartInitStruct.USART_Parity = USART_Parity_No;
    usartInitStruct.USART_StopBits = USART_StopBits_1;
    usartInitStruct.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART1, &usartInitStruct);

    USART_Cmd(USART1, ENABLE);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

    nvicInitStruct.NVIC_IRQChannel = USART1_IRQn;
    nvicInitStruct.NVIC_IRQChannelCmd = ENABLE;
    nvicInitStruct.NVIC_IRQChannelPreemptionPriority = 1;  /* ⭐ 抢占级1, 让出0给USART2(LoRa收), LoRa字节不能等 */
    nvicInitStruct.NVIC_IRQChannelSubPriority = 2;
    NVIC_Init(&nvicInitStruct);
}

/****************************************************************************
 * 函数名: Usart2_Init
 * 功能:   初始化串口2
 * 参数:   baud - 波特率
 * 返回值: 无
 * 引脚:   TX-PA2, RX-PA3
 * 用途:   与 LoRa 模块通信
 ****************************************************************************/
void Usart2_Init(unsigned int baud)
{
    GPIO_InitTypeDef gpioInitStruct;
    USART_InitTypeDef usartInitStruct;
    NVIC_InitTypeDef nvicInitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    gpioInitStruct.GPIO_Mode = GPIO_Mode_AF_PP;
    gpioInitStruct.GPIO_Pin = GPIO_Pin_2;
    gpioInitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpioInitStruct);

    gpioInitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpioInitStruct.GPIO_Pin = GPIO_Pin_3;
    gpioInitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpioInitStruct);

    usartInitStruct.USART_BaudRate = baud;
    usartInitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usartInitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usartInitStruct.USART_Parity = USART_Parity_No;
    usartInitStruct.USART_StopBits = USART_StopBits_1;
    usartInitStruct.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART2, &usartInitStruct);

    USART_Cmd(USART2, ENABLE);

    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    nvicInitStruct.NVIC_IRQChannel = USART2_IRQn;
    nvicInitStruct.NVIC_IRQChannelCmd = ENABLE;
    nvicInitStruct.NVIC_IRQChannelPreemptionPriority = 0;
    nvicInitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&nvicInitStruct);

    /* ⭐ USART2 发送改 DMA 非阻塞搬运(接收仍走 RXNE 中断) */
    Usart2_DmaTxInit();
}

/****************************************************************************
 * 函数名: Usart3_Init
 * 功能:   初始化串口3
 * 参数:   baud - 波特率
 * 返回值: 无
 * 引脚:   TX-PB10, RX-PB11
 * 用途:   与 ESP32-S3 摄像头模组通信(AT 从机, 115200)
 ****************************************************************************/
void Usart3_Init(unsigned int baud)
{
    GPIO_InitTypeDef gpioInitStruct;
    USART_InitTypeDef usartInitStruct;
    NVIC_InitTypeDef nvicInitStruct;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);

    gpioInitStruct.GPIO_Mode = GPIO_Mode_AF_PP;
    gpioInitStruct.GPIO_Pin = GPIO_Pin_10;
    gpioInitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpioInitStruct);

    gpioInitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpioInitStruct.GPIO_Pin = GPIO_Pin_11;
    gpioInitStruct.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &gpioInitStruct);

    usartInitStruct.USART_BaudRate = baud;
    usartInitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usartInitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usartInitStruct.USART_Parity = USART_Parity_No;
    usartInitStruct.USART_StopBits = USART_StopBits_1;
    usartInitStruct.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART3, &usartInitStruct);

    USART_Cmd(USART3, ENABLE);

    USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);

    nvicInitStruct.NVIC_IRQChannel = USART3_IRQn;
    nvicInitStruct.NVIC_IRQChannelCmd = ENABLE;
    nvicInitStruct.NVIC_IRQChannelPreemptionPriority = 2;  /* ⭐ 占先级2: 低于 USART2(0)/USART1(1), LoRa 收字节优先 */
    nvicInitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_Init(&nvicInitStruct);

    /* ⭐ USART3 发送改 DMA 非阻塞搬运(接收仍走 RXNE 中断) */
    Usart3_DmaTxInit();
}

/****************************************************************************
 * 函数名: Usart_Init
 * 功能:   初始化所有串口
 * 参数:   无
 * 返回值: 无
 * 说明:   USART1(115200)-调试串口; USART2 初值 115200,
 *         随后由 LoRa_Node_Init() 重新配置为 LORA_BAUD(9600)
 ****************************************************************************/
void Usart_Init(void)
{
    Usart1_Init(115200);
    Usart2_Init(115200);
}

/****************************************************************************
 * 函数名: Usart_SendString
 * 功能:   串口发送字符串
 * 参数:   USARTx - 串口号
 *         str - 要发送的数据指针
 *         len - 数据长度
 * 返回值: 无
 ****************************************************************************/
void Usart_SendString(USART_TypeDef *USARTx, unsigned char *str, unsigned short len)
{
    unsigned short count = 0;
    uint32_t startTick;

    for (; count < len; count++)
    {
        USART_SendData(USARTx, *str++);
        /* ⭐ 死等TC加超时: 50ms未发送完成直接跳过, 防止串口硬件异常卡死主循环 */
        startTick = Get_Tick();
        while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET)
        {
            if (Get_Tick() - startTick > USART_SEND_TIMEOUT_MS)
                break;
        }
    }
}

/****************************************************************************
 * 函数名: Usart2_SendAsync
 * 功能:   USART2 非阻塞发送(DMA1_Channel7 搬运), 启动后立即返回
 * 参数:   data - 待发送数据指针(会先拷入内部静态缓冲)
 *         len  - 数据长度(超过 USART2_TXBUF_SIZE 则截断)
 * 返回值: 无
 * 说明:   ⭐ DMA 搬运期间源缓冲必须保持有效, 故此处先 memcpy 到内部缓冲,
 *         调用者的缓冲可立即复用或离开作用域.
 *         若上一帧尚未搬完, 会先等它结束(节点每轮才回一帧, 正常为 0ms).
 *         返回不代表"已发完", 只代表"已开始发送".
 ****************************************************************************/
void Usart2_SendAsync(const uint8_t *data, uint16_t len)
{
    uint32_t t0;

    if (data == 0 || len == 0)
        return;

    if (len > USART2_TXBUF_SIZE)
        len = USART2_TXBUF_SIZE;   /* 协议帧最长 70B, 此处仅防御性截断 */

    /* 上一帧还没搬完 -> 先等结束, 否则会覆写正在被 DMA 读取的缓冲 */
    t0 = Get_Tick();
    while (Usart2_TxBusy() &&
           (Get_Tick() - t0) <= USART2_TX_DMA_WAIT_MS) { }

    memcpy(s_usart2_txbuf, data, len);

    /* 改 CNDTR 前必须先关通道; Normal 模式下传完会自动关通道 */
    DMA_Cmd(USART2_TX_DMA_CHANNEL, DISABLE);
    DMA_ClearFlag(USART2_TX_DMA_FLAG_TC | USART2_TX_DMA_FLAG_GL);
    DMA_SetCurrDataCounter(USART2_TX_DMA_CHANNEL, len);
    DMA_Cmd(USART2_TX_DMA_CHANNEL, ENABLE);
}

/****************************************************************************
 * 函数名: Usart2_TxBusy
 * 功能:   查询上一帧 DMA 是否仍在发送中
 * 参数:   无
 * 返回值: 1=DMA 仍在搬运; 0=已完成/空闲
 ****************************************************************************/
uint8_t Usart2_TxBusy(void)
{
    /* 本 SPL 版本无 DMA_GetCmdStatus, 用剩余传输数判断:
     * 搬运中 CNDTR>0; 传输完成(Normal 模式自动关通道)或未启动时 = 0 */
    return (DMA_GetCurrDataCounter(USART2_TX_DMA_CHANNEL) != 0) ? 1 : 0;
}

/****************************************************************************
 * 函数名: Usart3_SendAsync
 * 功能:   USART3 非阻塞发送(DMA1_Channel2 搬运), 启动后立即返回
 * 参数:   data - 待发送数据指针(会先拷入内部静态缓冲)
 *         len  - 数据长度(超过 USART3_TXBUF_SIZE 则截断)
 * 返回值: 无
 ****************************************************************************/
void Usart3_SendAsync(const uint8_t *data, uint16_t len)
{
    uint32_t t0;

    if (data == 0 || len == 0)
        return;

    if (len > USART3_TXBUF_SIZE)
        len = USART3_TXBUF_SIZE;

    /* 上一帧还没搬完 -> 先等结束, 否则会覆写正在被 DMA 读取的缓冲 */
    t0 = Get_Tick();
    while (Usart3_TxBusy() &&
           (Get_Tick() - t0) <= USART3_TX_DMA_WAIT_MS) { }

    memcpy(s_usart3_txbuf, data, len);

    /* 改 CNDTR 前必须先关通道; Normal 模式下传完会自动关通道 */
    DMA_Cmd(USART3_TX_DMA_CHANNEL, DISABLE);
    DMA_ClearFlag(USART3_TX_DMA_FLAG_TC | USART3_TX_DMA_FLAG_GL);
    DMA_SetCurrDataCounter(USART3_TX_DMA_CHANNEL, len);
    DMA_Cmd(USART3_TX_DMA_CHANNEL, ENABLE);
}

/****************************************************************************
 * 函数名: Usart3_TxBusy
 * 功能:   查询上一帧 DMA 是否仍在发送中
 * 参数:   无
 * 返回值: 1=DMA 仍在搬运; 0=已完成/空闲
 ****************************************************************************/
uint8_t Usart3_TxBusy(void)
{
    return (DMA_GetCurrDataCounter(USART3_TX_DMA_CHANNEL) != 0) ? 1 : 0;
}

/****************************************************************************
 * 函数名: Usart_Printf
 * 功能:   串口格式化打印
 * 参数:   USARTx - 串口号
 *         fmt - 格式化字符串
 *         ... - 可变参数
 * 返回值: 无
 * 说明:   类似于标准库printf,支持格式化输出到串口
 ****************************************************************************/
void Usart_Printf(USART_TypeDef *USARTx, char *fmt, ...)
{
    char buf[296];
    char *p = buf;
    va_list ap;
    uint32_t startTick;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = '\0';
    va_end(ap);

    while (*p)
    {
        /* ⭐ 死等TXE加超时: 50ms未就绪直接跳过当前字节 */
        startTick = Get_Tick();
        while (USART_GetFlagStatus(USARTx, USART_FLAG_TXE) == RESET)
        {
            if (Get_Tick() - startTick > USART_SEND_TIMEOUT_MS)
                break;
        }
        USART_SendData(USARTx, (uint8_t)(*p++));
    }

    /* ⭐ 最终死等TC加超时 */
    startTick = Get_Tick();
    while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET)
    {
        if (Get_Tick() - startTick > USART_SEND_TIMEOUT_MS)
            break;
    }
}

/****************************************************************************
 * 函数名: USART1_IRQHandler
 * 功能:   串口1接收中断服务函数
 * 参数:   无
 * 返回值: 无
 ****************************************************************************/
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        USART_ClearFlag(USART1, USART_FLAG_RXNE);
    }
}

/****************************************************************************
 * 函数名: USART2_IRQHandler
 * 功能:   串口2接收中断服务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   接收到的数据存入环形缓冲区(usart2_rbuf)
 *         供 LoRa 模块读取使用
 ****************************************************************************/
/* ORE 溢出计数器，用于调试 */
volatile uint16_t usart2_oreCount = 0;
/* ISR 接收字节计数器，用于性能监测 */
volatile uint32_t usart2_rxCount = 0;

void USART2_IRQHandler(void)
{
    /* ⭐ 顺序修复: 必须先处理 RXNE 再处理 ORE.
     * 旧代码先查 ORE, ORE 置位时读 DR 清 ORE 会顺便清掉 RXNE,
     * 导致当前字节被丢弃 -> 命令残缺(如 AT+PING 丢 G 变 AT+PIN),
     * 喂给上层 s_rxBuf 凑不成 \r\n -> 最终触发缓冲区死锁, 节点掉线.
     *
     * STM32F1 ORE/RXNE 正确处理:
     *   - ORE 置位时 RXNE 可能同时置位(新字节已在 DR)
     *   - 先读 RXNE: 读 DR 取走新字节 + 顺带清 RXNE 和 ORE
     *   - 若仍残留 ORE(如纯 overrun 无新字节): 读 SR+读 DR 清除
     */
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint16_t nextHead;
        uint8_t data;

        data = (uint8_t)USART_ReceiveData(USART2);  /* 读 DR: 取数据 + 清 RXNE + 清 ORE */

        nextHead = (usart2_rhead + 1) % USART2_RBUF_SIZE;
        if (nextHead == usart2_rtail)
        {
            /* 缓冲区满: 丢弃新数据(保持tail不动) */
        }
        else
        {
            usart2_rbuf[usart2_rhead] = data;
            usart2_rhead = nextHead;
            usart2_rxCount++;
        }
    }

    /* ORE 残留兜底: 上面读 DR 已清掉大多数 ORE,
     * 但纯 overrun(无新字节) 时 RXNE 不会置位, 需这里补清,
     * 否则 ORE 一直置位会阻止后续 RXNE 中断产生 -> 串口"卡死" */
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
    {
        usart2_oreCount++;
        (void)USART_ReceiveData(USART2);   /* 读 DR 清 ORE */
        /* USART_ClearFlag 对 ORE 在 F1 上实际无效(ORE 为 rc_w0/read-clear),
         * 但保留以兼容其他 STM32 系列 */
        USART_ClearFlag(USART2, USART_FLAG_ORE);
    }
}

/****************************************************************************
 * 函数名: USART3_IRQHandler
 * 功能:   串口3接收中断服务函数(摄像头模组)
 * 参数:   无
 * 返回值: 无
 * 说明:   接收到的数据存入环形缓冲区(usart3_rbuf), 供 app_plate 读取.
 *         处理顺序与 USART2 一致: 先 RXNE 再兜底清 ORE(见 USART2 注释)
 ****************************************************************************/
volatile uint16_t usart3_oreCount = 0;
volatile uint32_t usart3_rxCount = 0;

void USART3_IRQHandler(void)
{
    if (USART_GetITStatus(USART3, USART_IT_RXNE) != RESET)
    {
        uint16_t nextHead;
        uint8_t data;

        data = (uint8_t)USART_ReceiveData(USART3);  /* 读 DR: 取数据 + 清 RXNE + 清 ORE */

        nextHead = (usart3_rhead + 1) % USART3_RBUF_SIZE;
        if (nextHead == usart3_rtail)
        {
            /* 缓冲区满: 丢弃新数据(保持tail不动) */
        }
        else
        {
            usart3_rbuf[usart3_rhead] = data;
            usart3_rhead = nextHead;
            usart3_rxCount++;
        }
    }

    /* ORE 残留兜底(纯 overrun 无新字节时 RXNE 不置位) */
    if (USART_GetFlagStatus(USART3, USART_FLAG_ORE) != RESET)
    {
        usart3_oreCount++;
        (void)USART_ReceiveData(USART3);
        USART_ClearFlag(USART3, USART_FLAG_ORE);
    }
}

/****************************************************************************
 * 函数名: Usart2_GetData
 * 功能:   从 USART2 环形缓冲区读取数据 (非阻塞)
 * 参数:   buf    - 接收缓冲区
 *         maxlen - 最大读取长度
 * 返回值: 实际读取的字节数 (0=缓冲区空)
 ****************************************************************************/
uint16_t Usart2_GetData(uint8_t *buf, uint16_t maxlen)
{
    uint16_t count = 0;

    while (usart2_rhead != usart2_rtail && count < maxlen)
    {
        buf[count++] = usart2_rbuf[usart2_rtail];
        usart2_rtail = (usart2_rtail + 1) % USART2_RBUF_SIZE;
    }

    return count;
}

/****************************************************************************
 * 函数名: Usart3_GetData
 * 功能:   从 USART3 环形缓冲区读取数据 (非阻塞)
 * 参数:   buf    - 接收缓冲区
 *         maxlen - 最大读取长度
 * 返回值: 实际读取的字节数 (0=缓冲区空)
 ****************************************************************************/
uint16_t Usart3_GetData(uint8_t *buf, uint16_t maxlen)
{
    uint16_t count = 0;

    while (usart3_rhead != usart3_rtail && count < maxlen)
    {
        buf[count++] = usart3_rbuf[usart3_rtail];
        usart3_rtail = (usart3_rtail + 1) % USART3_RBUF_SIZE;
    }

    return count;
}

/****************************************************************************
 * 函数名: Usart3_FlushRx
 * 功能:   清空 USART3 接收环形缓冲
 * 参数:   无
 * 返回值: 无
 * 说明:   发新命令前调用, 丢弃残留回显/上一轮未取走的结果块,
 *         避免旧数据被误解析成本轮结果
 ****************************************************************************/
void Usart3_FlushRx(void)
{
    usart3_rtail = usart3_rhead;
}
