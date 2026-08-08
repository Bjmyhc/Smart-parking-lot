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

/* ==================== USART2 接收环形缓冲 ==================== */
static volatile uint8_t  usart2_rbuf[USART2_RBUF_SIZE];
static volatile uint16_t usart2_rhead = 0;    /* 写入位置 */
static volatile uint16_t usart2_rtail = 0;    /* 读取位置 */

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
    nvicInitStruct.NVIC_IRQChannelPreemptionPriority = 0;
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

    for (; count < len; count++)
    {
        USART_SendData(USARTx, *str++);
        while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET);
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

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = '\0';
    va_end(ap);

    while (*p)
    {
        while (USART_GetFlagStatus(USARTx, USART_FLAG_TXE) == RESET);
        USART_SendData(USARTx, (uint8_t)(*p++));
    }

    while (USART_GetFlagStatus(USARTx, USART_FLAG_TC) == RESET);
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
        /* 清空中断标志, 调试串口丢弃接收字节 */
        (void)USART_ReceiveData(USART1);
        USART_ClearFlag(USART1, USART_FLAG_RXNE);
    }
}

/****************************************************************************
 * 函数名: USART2_IRQHandler
 * 功能:   串口2接收中断服务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   每个收到的字节存入环形缓冲(usart2_rbuf)供上层读取
 *         用于 LoRa 模块下行数据接收
 ****************************************************************************/
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint16_t nextHead;
        uint8_t data;

        data = (uint8_t)USART_ReceiveData(USART2);

        nextHead = (usart2_rhead + 1) % USART2_RBUF_SIZE;
        if (nextHead == usart2_rtail)
        {
            /* 缓冲满: 丢弃新字节(避免覆盖tail正在读的数据) */
        }
        else
        {
            usart2_rbuf[usart2_rhead] = data;
            usart2_rhead = nextHead;
        }

        USART_ClearFlag(USART2, USART_FLAG_RXNE);
    }
}

/****************************************************************************
 * 函数名: Usart2_GetData
 * 功能:   从 USART2 接收环形缓冲取出数据 (非阻塞)
 * 参数:   buf    - 目标缓冲区
 *         maxlen - 最大读取字节数
 * 返回值: 实际读取字节数 (0=无数据)
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
