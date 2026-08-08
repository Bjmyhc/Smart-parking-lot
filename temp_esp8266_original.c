/****************************************************************************
 * ESP8266 WiFi模块驱动 - esp8266.c
 * 
 * 功能描述:
 *   实现ESP8266 WiFi模块的初始化、数据收发、AT指令解析等功能
 *   通过USART2与ESP8266通信
 *   支持WiFi连接、TCP连接、数据透传等功能
 * 
 * 硬件配置:
 *   - USART2: TX-PA2, RX-PA3, 波特率115200
 *   - ESP8266模块: WiFi连接、TCP透传模式
 * 
 * 缓冲区机制:
 *   - 环形缓冲区: 用于中断接收数据,防止数据丢失
 *   - 线性缓冲区: 用于数据处理和解析
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "stm32f10x.h"
#include "esp8266.h"
#include "bsp_delay.h"
#include "bsp_usart.h"
#include <string.h>
#include <stdio.h>
#include "device_config.h"

/* WiFi账号和密码(引用device_config.h中的配置) */
#define ESP8266_WIFI_INFO     "AT+CWJAP=\"" WIFI_SSID "\",\"" WIFI_PWD "\"\r\n"

/* OneNET平台TCP连接信息 */
#define ESP8266_ONENET_INFO   "AT+CIPSTART=\"TCP\",\"mqtts.heclouds.com\",1883\r\n"

/* 环形缓冲区 */
static unsigned char esp8266_buf[ESP8266_BUF_SIZE];
static ESP8266_RxBuffer_t esp8266_rx = {0, 0, 0};

/* 线性缓冲区（用于数据处理） */
static unsigned char esp8266_lineBuf[ESP8266_BUF_SIZE];
static uint16_t esp8266_lineLen = 0;

/* 上次接收时间（用于判断接收完成） */
static uint32_t lastRxTick = 0;
#define RX_TIMEOUT_MS    10

/****************************************************************************
 * 函数名: ESP8266_Clear
 * 功能:   清空ESP8266接收缓冲区
 * 参数:   无
 * 返回值: 无
 * 说明:   清空环形缓冲区和线性缓冲区,包括头指针、尾指针、溢出标志
 ****************************************************************************/
void ESP8266_Clear(void)
{
    __disable_irq();
    esp8266_rx.head = 0;
    esp8266_rx.tail = 0;
    esp8266_rx.overflow = 0;
    esp8266_lineLen = 0;
    memset(esp8266_lineBuf, 0, sizeof(esp8266_lineBuf));
    __enable_irq();
}

/****************************************************************************
 * 函数名: ESP8266_GetDataLen
 * 功能:   获取环形缓冲区中数据长度
 * 参数:   无
 * 返回值: 当前缓冲区中数据字节数
 * 说明:   支持环形缓冲区的正常和回绕两种情况
 ****************************************************************************/
uint16_t ESP8266_GetDataLen(void)
{
    uint16_t len;
    __disable_irq();
    if (esp8266_rx.head >= esp8266_rx.tail)
        len = esp8266_rx.head - esp8266_rx.tail;
    else
        len = ESP8266_BUF_SIZE - esp8266_rx.tail + esp8266_rx.head;
    __enable_irq();
    return len;
}

/****************************************************************************
 * 函数名: ESP8266_IsOverflow
 * 功能:   检查缓冲区是否溢出
 * 参数:   无
 * 返回值: 1-溢出, 0-正常
 * 说明:   当缓冲区满时,新数据会被丢弃并设置溢出标志
 ****************************************************************************/
uint8_t ESP8266_IsOverflow(void)
{
    return esp8266_rx.overflow;
}

/****************************************************************************
 * 函数名: ESP8266_ClearOverflow
 * 功能:   清除缓冲区溢出标志
 * 参数:   无
 * 返回值: 无
 * 说明:   在处理完溢出情况后调用此函数清除标志
 ****************************************************************************/
void ESP8266_ClearOverflow(void)
{
    esp8266_rx.overflow = 0;
}

/****************************************************************************
 * 函数名: ESP8266_ReadByte
 * 功能:   从环形缓冲区读取一个字节
 * 参数:   无
 * 返回值: 读取的字节值, -1表示缓冲区为空
 * 说明:   内部函数,需保证在临界区调用
 ****************************************************************************/
static int16_t ESP8266_ReadByte(void)
{
    uint16_t data;

    if (esp8266_rx.head == esp8266_rx.tail)
        return -1;

    __disable_irq();
    data = esp8266_buf[esp8266_rx.tail];
    esp8266_rx.tail = (esp8266_rx.tail + 1) % ESP8266_BUF_SIZE;
    __enable_irq();

    return data;
}

/****************************************************************************
 * 函数名: ESP8266_WaitRecive
 * 功能:   等待数据接收完成
 * 参数:   无
 * 返回值: REV_OK-接收完成, REV_WAIT-接收未完成
 * 说明:   使用超时机制判断接收完成(10ms无新数据)
 ****************************************************************************/
_Bool ESP8266_WaitRecive(void)
{
    uint32_t checkTick = Get_Tick();

    if (ESP8266_GetDataLen() == 0)
        return REV_WAIT;

    if (checkTick - lastRxTick >= RX_TIMEOUT_MS)
    {
        lastRxTick = checkTick;
        return REV_OK;
    }

    return REV_WAIT;
}

/****************************************************************************
 * 函数名: ESP8266_CopyToLineBuf
 * 功能:   将环形缓冲区数据复制到线性缓冲区
 * 参数:   无
 * 返回值: 复制的数据长度
 * 说明:   将环形缓冲区中的数据复制到线性缓冲区,并添加字符串结束符
 ****************************************************************************/
static uint16_t ESP8266_CopyToLineBuf(void)
{
    uint16_t len = ESP8266_GetDataLen();
    uint16_t i;
    int16_t data;

    esp8266_lineLen = 0;

    for (i = 0; i < len && i < ESP8266_BUF_SIZE - 1; i++)
    {
        data = ESP8266_ReadByte();
        if (data < 0)
            break;
        esp8266_lineBuf[esp8266_lineLen++] = (uint8_t)data;
    }
    esp8266_lineBuf[esp8266_lineLen] = '\0';

    return esp8266_lineLen;
}

/****************************************************************************
 * 函数名: ESP8266_SendCmd
 * 功能:   发送AT指令并等待响应
 * 参数:   cmd - 要发送的AT指令
 *         res - 期望的响应关键词
 * 返回值: 0-成功(收到期望响应), 1-失败(超时或未收到期望响应)
 * 说明:   超时时间约2秒(200次×10ms)
 ****************************************************************************/
_Bool ESP8266_SendCmd(char *cmd, char *res)
{
    unsigned char timeOut = 200;

    ESP8266_Clear();
    Usart_SendString(USART2, (unsigned char *)cmd, strlen((const char *)cmd));

    while (timeOut--)
    {
        if (ESP8266_WaitRecive() == REV_OK)
        {
            ESP8266_CopyToLineBuf();
            if (strstr((const char *)esp8266_lineBuf, res) != NULL)
            {
                ESP8266_Clear();
                return 0;
            }
        }

        DelayXms(10);
    }

    return 1;
}

/****************************************************************************
 * 函数名: ESP8266_SendData
 * 功能:   通过ESP8266发送数据
 * 参数:   data - 数据指针
 *         len - 数据长度
 * 返回值: 无
 * 说明:   使用AT+CIPSEND命令发送数据,等待">"后发送实际数据
 ****************************************************************************/
void ESP8266_SendData(unsigned char *data, unsigned short len)
{
    char cmdBuf[32];

    ESP8266_Clear();
    sprintf(cmdBuf, "AT+CIPSEND=%d\r\n", len);
    if (!ESP8266_SendCmd(cmdBuf, ">"))
    {
        Usart_SendString(USART2, data, len);
    }
}

/****************************************************************************
 * 函数名: ESP8266_GetIPD
 * 功能:   获取平台返回的数据(非阻塞)
 * 参数:   timeOut - 保留参数(未使用,兼容旧接口)
 * 返回值: 平台返回的原始数据指针(跳过IPD头), NULL表示暂无数据
 * 说明:   ESP8266返回格式: "+IPD,x:yyy"
 *         x是数据长度, yyy是实际数据内容
 *         非阻塞版本,每次调用立即返回,不再DelayXms等待
 ****************************************************************************/
unsigned char *ESP8266_GetIPD(unsigned short timeOut)
{
    char *ptrIPD = NULL;

    if (ESP8266_WaitRecive() == REV_OK)
    {
        ESP8266_CopyToLineBuf();
        ptrIPD = strstr((char *)esp8266_lineBuf, "IPD,");
        if (ptrIPD != NULL)
        {
            ptrIPD = strchr(ptrIPD, ':');
            if (ptrIPD != NULL)
            {
                ptrIPD++;
                return (unsigned char *)(ptrIPD);
            }
        }
    }

    return NULL;
}

/****************************************************************************
 * 函数名: ESP8266_Init
 * 功能:   初始化ESP8266 WiFi模块
 * 参数:   无
 * 返回值: 无
 * 说明:   初始化流程:
 *         1. AT测试 - 检查模块是否正常
 *         2. CWMODE - 设置STA模式
 *         3. CWDHCP - 开启DHCP
 *         4. CWJAP - 连接WiFi
 *         5. CIPSTART - 连接OneNET平台
 ****************************************************************************/
void ESP8266_Init(void)
{
    ESP8266_Clear();

    Usart_Printf(USART_DEBUG, "1. AT\r\n");
    while (ESP8266_SendCmd("AT\r\n", "OK"))
        DelayXms(500);

    Usart_Printf(USART_DEBUG, "2. CWMODE\r\n");
    while (ESP8266_SendCmd("AT+CWMODE=1\r\n", "OK"))
        DelayXms(500);

    Usart_Printf(USART_DEBUG, "3. AT+CWDHCP\r\n");
    while (ESP8266_SendCmd("AT+CWDHCP=1,1\r\n", "OK"))
        DelayXms(500);

    Usart_Printf(USART_DEBUG, "4. CWJAP\r\n");
    while (ESP8266_SendCmd(ESP8266_WIFI_INFO, "GOT IP"))
        DelayXms(500);

    Usart_Printf(USART_DEBUG, "5. CIPSTART\r\n");
    while (ESP8266_SendCmd(ESP8266_ONENET_INFO, "CONNECT"))
        DelayXms(500);

    Usart_Printf(USART_DEBUG, "6. ESP8266 Init OK\r\n");
}

/****************************************************************************
 * 函数名: USART2_IRQHandler
 * 功能:   串口2接收中断服务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   将接收到的数据写入环形缓冲区
 *         检查缓冲区溢出,更新最后接收时间戳
 ****************************************************************************/
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint16_t nextHead;
        uint8_t data;

        data = USART2->DR;

        nextHead = (esp8266_rx.head + 1) % ESP8266_BUF_SIZE;

        if (nextHead == esp8266_rx.tail)
        {
            esp8266_rx.overflow = 1;
        }
        else
        {
            esp8266_buf[esp8266_rx.head] = data;
            esp8266_rx.head = nextHead;
            lastRxTick = Get_Tick();
        }

        USART_ClearFlag(USART2, USART_FLAG_RXNE);
    }
}
