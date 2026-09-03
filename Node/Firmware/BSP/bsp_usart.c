/****************************************************************************
 * 串口驱动 - bsp_usart.c
 * 
 * 功能描述:
 *   实现USART1和USART2的初始化配置、数据发送、格式化打印等功能
 *   USART1用于调试输出，USART2用于与ESP8266模块通信
 * 
 * 硬件配置:
 *   - USART1: TX-PA9, RX-PA10, 波特率115200
 *   - USART2: TX-PA2, RX-PA3, 波特率115200
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "bsp_usart.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "bsp_delay.h"   /* ⭐ 引入 Get_Tick() 用于串口发送超时判断 */

/* ==================== 串口发送超时(ms): 硬件异常时最多阻塞 50ms, 避免死等卡死主循环 ==================== */
#define USART_SEND_TIMEOUT_MS 50

/* ==================== USART2 环形缓冲区变量 ==================== */
static volatile uint8_t  usart2_rbuf[USART2_RBUF_SIZE];
static volatile uint16_t usart2_rhead = 0;    /* 写入指针 */
static volatile uint16_t usart2_rtail = 0;    /* 读取指针 */

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
 * 用途:   与ESP8266 WiFi模块通信
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
}

/****************************************************************************
 * 函数名: Usart_Init
 * 功能:   初始化所有串口
 * 参数:   无
 * 返回值: 无
 * 说明:   USART1(115200)-调试串口, USART2(115200)-ESP8266通信串口
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
