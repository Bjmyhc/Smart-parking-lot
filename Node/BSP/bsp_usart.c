/****************************************************************************
 * ???????? - bsp_usart.c
 * 
 * ????????:
 *   ???USART1??USART2?????????¨¢??????????????????????
 *   USART1????????????USART2??????ESP8266??????
 * 
 * ???????:
 *   - USART1: TX-PA9, RX-PA10, ??????115200
 *   - USART2: TX-PA2, RX-PA3, ??????115200
 * 
 * ????: Bjmyhc
 * ????: 2026-07-19
 ****************************************************************************/

#include "bsp_usart.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* ==================== USART2 ??????¦Ë??? ==================== */
static volatile uint8_t  usart2_rbuf[USART2_RBUF_SIZE];
static volatile uint16_t usart2_rhead = 0;    /* §Õ??¦Ë?? */
static volatile uint16_t usart2_rtail = 0;    /* ???¦Ë?? */

/****************************************************************************
 * ??????: Usart1_Init
 * ????:   ?????????1
 * ????:   baud - ??????
 * ?????: ??
 * ????:   TX-PA9, RX-PA10
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
 * ??????: Usart2_Init
 * ????:   ?????????2
 * ????:   baud - ??????
 * ?????: ??
 * ????:   TX-PA2, RX-PA3
 * ???:   ??ESP8266 WiFi??????
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
 * ??????: Usart_Init
 * ????:   ????????§Õ???
 * ????:   ??
 * ?????: ??
 * ???:   USART1(115200)-???????, USART2(115200)-ESP8266??????
 ****************************************************************************/
void Usart_Init(void)
{
    Usart1_Init(115200);
    Usart2_Init(115200);
}

/****************************************************************************
 * ??????: Usart_SendString
 * ????:   ????????????
 * ????:   USARTx - ?????
 *         str - ?????????????
 *         len - ???????
 * ?????: ??
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
 * ??????: Usart_Printf
 * ????:   ???????????
 * ????:   USARTx - ?????
 *         fmt - ??????????
 *         ... - ??????
 * ?????: ??
 * ???:   ??????????printf,????????????????
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
 * ??????: USART1_IRQHandler
 * ????:   ????1?????§Ø??????
 * ????:   ??
 * ?????: ??
 ****************************************************************************/
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        /* ????§Ø???, ????????????????? */
        (void)USART_ReceiveData(USART1);
        USART_ClearFlag(USART1, USART_FLAG_RXNE);
    }
}

/****************************************************************************
 * ??????: USART2_IRQHandler
 * ????:   ????2?????§Ø??????
 * ????:   ??
 * ?????: ??
 * ???:   ???????????????¦Ë???(usart2_rbuf)???????
 *         ???? LoRa ??????????????
 ****************************************************************************/
/* ORE ?????????, ???????? */
volatile uint16_t usart2_oreCount = 0;
/* ISR ???????????, ??????§Ø? ISR ???????? */
volatile uint32_t usart2_rxCount = 0;

void USART2_IRQHandler(void)
{
    /* Overrun Error ????: ORE ??¦Ë?? RXNE ?§Ø???????,
     * ?????? ORE ??????????, ???? USART2 ????"????" */
    if (USART_GetFlagStatus(USART2, USART_FLAG_ORE) != RESET)
    {
        usart2_oreCount++;
        /* STM32F1 ?? ORE: ?? SR(???? GetFlagStatus ???) ??? DR */
        (void)USART_ReceiveData(USART2);
        USART_ClearFlag(USART2, USART_FLAG_ORE);
    }

    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint16_t nextHead;
        uint8_t data;

        data = (uint8_t)USART_ReceiveData(USART2);

        nextHead = (usart2_rhead + 1) % USART2_RBUF_SIZE;
        if (nextHead == usart2_rtail)
        {
            /* ??????: ?????????(??????tail???????????) */
        }
        else
        {
            usart2_rbuf[usart2_rhead] = data;
            usart2_rhead = nextHead;
            usart2_rxCount++;
        }

        USART_ClearFlag(USART2, USART_FLAG_RXNE);
    }
}

/****************************************************************************
 * ??????: Usart2_GetData
 * ????:   ?? USART2 ??????¦Ë?????????? (??????)
 * ????:   buf    - ???????
 *         maxlen - ??????????
 * ?????: ?????????? (0=??????)
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
