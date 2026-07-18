

//单片机头文件
#include "stm32f10x.h"

//网络设备
#include "esp8266.h"

//硬件驱动
#include "bsp_delay.h"
#include "bsp_usart.h"

//C库
#include <string.h>
#include <stdio.h>


#define ESP8266_WIFI_INFO		"AT+CWJAP=\"Aira\",\"Zdgdzl934395.\"\r\n"//wifi账号和密码

#define ESP8266_ONENET_INFO		"AT+CIPSTART=\"TCP\",\"mqtts.heclouds.com\",1883\r\n"

/* 环形缓冲区 */
static unsigned char esp8266_buf[ESP8266_BUF_SIZE];
static ESP8266_RxBuffer_t esp8266_rx = {0, 0, 0};

/* 线性缓冲区（用于数据处理） */
static unsigned char esp8266_lineBuf[ESP8266_BUF_SIZE];
static uint16_t esp8266_lineLen = 0;

/* 上次接收时间（用于判断接收完成） */
static uint32_t lastRxTick = 0;
#define RX_TIMEOUT_MS	10	// 10ms无新数据认为接收完成


//==========================================================
//	函数名称：	ESP8266_Clear
//
//	函数功能：	清空缓存
//
//	入口参数：	无
//
//	返回参数：	无
//
//	说明：		
//==========================================================
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

//==========================================================
//	函数名称：	ESP8266_GetDataLen
//
//	函数功能：	获取缓冲区中数据长度
//
//	入口参数：	无
//
//	返回参数：	数据长度
//
//	说明：		
//==========================================================
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

//==========================================================
//	函数名称：	ESP8266_IsOverflow
//
//	函数功能：	检查是否溢出
//
//	入口参数：	无
//
//	返回参数：	1-溢出  0-正常
//
//	说明：		
//==========================================================
uint8_t ESP8266_IsOverflow(void)
{
	return esp8266_rx.overflow;
}

//==========================================================
//	函数名称：	ESP8266_ClearOverflow
//
//	函数功能：	清除溢出标志
//
//	入口参数：	无
//
//	返回参数：	无
//
//	说明：		
//==========================================================
void ESP8266_ClearOverflow(void)
{
	esp8266_rx.overflow = 0;
}

//==========================================================
//	函数名称：	ESP8266_ReadByte
//
//	函数功能：	从环形缓冲区读取一个字节
//
//	入口参数：	无
//
//	返回参数：	读取的字节，-1表示无数据
//
//	说明：		
//==========================================================
static int16_t ESP8266_ReadByte(void)
{
	uint16_t data;
	
	if (esp8266_rx.head == esp8266_rx.tail)
		return -1;	// 无数据
	
	__disable_irq();
	data = esp8266_buf[esp8266_rx.tail];
	esp8266_rx.tail = (esp8266_rx.tail + 1) % ESP8266_BUF_SIZE;
	__enable_irq();
	
	return data;
}

//==========================================================
//	函数名称：	ESP8266_WaitRecive
//
//	函数功能：	等待接收完成
//
//	入口参数：	无
//
//	返回参数：	REV_OK-接收完成		REV_WAIT-接收未完成
//
//	说明：		使用超时判断接收完成
//==========================================================
_Bool ESP8266_WaitRecive(void)
{
	static uint32_t checkTick = 0;
	
	// 无数据
	if (ESP8266_GetDataLen() == 0)
		return REV_WAIT;
	
	// 检查是否超时（10ms无新数据）
	if (Get_Tick() - lastRxTick >= RX_TIMEOUT_MS)
	{
		return REV_OK;
	}
	
	return REV_WAIT;
}

//==========================================================
//	函数名称：	ESP8266_CopyToLineBuf
//
//	函数功能：	将环形缓冲区数据复制到线性缓冲区
//
//	入口参数：	无
//
//	返回参数：	数据长度
//
//	说明：		
//==========================================================
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

//==========================================================
//	函数名称：	ESP8266_SendCmd
//
//	函数功能：	发送命令
//
//	入口参数：	cmd：命令
//				res：需要检查的返回指令
//
//	返回参数：	0-成功	1-失败
//
//	说明：		
//==========================================================
_Bool ESP8266_SendCmd(char *cmd, char *res)
{
	
	unsigned char timeOut = 200;

	ESP8266_Clear();
	Usart_SendString(USART2, (unsigned char *)cmd, strlen((const char *)cmd));
	
	while(timeOut--)
	{
		if(ESP8266_WaitRecive() == REV_OK)							//如果接收完成
		{
			ESP8266_CopyToLineBuf();
			if(strstr((const char *)esp8266_lineBuf, res) != NULL)		//如果检索到关键词
			{
				ESP8266_Clear();									//清空缓存
				return 0;
			}
		}
		
		DelayXms(10);
	}
	
	return 1;

}

//==========================================================
//	函数名称：	ESP8266_SendData
//
//	函数功能：	发送数据
//
//	入口参数：	data：数据
//				len：长度
//
//	返回参数：	无
//
//	说明：		
//==========================================================
void ESP8266_SendData(unsigned char *data, unsigned short len)
{

	char cmdBuf[32];
	
	ESP8266_Clear();								//清空接收缓存
	sprintf(cmdBuf, "AT+CIPSEND=%d\r\n", len);		//发送命令
	if(!ESP8266_SendCmd(cmdBuf, ">"))				//收到>时即可发送数据
	{
		Usart_SendString(USART2, data, len);		//发送设备数据
	}

}

//==========================================================
//	函数名称：	ESP8266_GetIPD
//
//	函数功能：	获取平台返回的数据
//
//	入口参数：	等待时间(单位10ms)
//
//	返回参数：	平台返回的原始数据
//
//	说明：		不同网络设备返回的格式不同，需要去解析
//				ESP8266的返回格式为	"+IPD,x:yyy"	x是数据长度，yyy是数据内容
//==========================================================
unsigned char *ESP8266_GetIPD(unsigned short timeOut)
{

	char *ptrIPD = NULL;
	
	do
	{
		if(ESP8266_WaitRecive() == REV_OK)								//如果接收完成
		{
			ESP8266_CopyToLineBuf();
			ptrIPD = strstr((char *)esp8266_lineBuf, "IPD,");			//搜索"IPD"头
			if(ptrIPD == NULL)											//如果没有找到
			{
				// 不打印调试信息，避免阻塞
			}
			else
			{
				ptrIPD = strchr(ptrIPD, ':');							//找到':'
				if(ptrIPD != NULL)
				{
					ptrIPD++;
					return (unsigned char *)(ptrIPD);
				}
				else
					return NULL;
				
			}
		}
		
		DelayXms(5);
		timeOut--;		//超时等待
	} while(timeOut > 0);
	
	return NULL;														//超时还未找到，返回空指针

}

//==========================================================
//	函数名称：	ESP8266_Init
//
//	函数功能：	初始化ESP8266
//
//	入口参数：	无
//
//	返回参数：	无
//
//	说明：		
//==========================================================
void ESP8266_Init(void)
{
		
		ESP8266_Clear();
	
		Usart_Printf(USART_DEBUG, "1. AT\r\n");
		while(ESP8266_SendCmd("AT\r\n", "OK"))
		DelayXms(500);
		
		Usart_Printf(USART_DEBUG, "2. CWMODE\r\n");
		while(ESP8266_SendCmd("AT+CWMODE=1\r\n", "OK"))
		DelayXms(500);
	
		Usart_Printf(USART_DEBUG, "3. AT+CWDHCP\r\n");
		while(ESP8266_SendCmd("AT+CWDHCP=1,1\r\n", "OK"))
		DelayXms(500);
	
		Usart_Printf(USART_DEBUG, "4. CWJAP\r\n");
		while(ESP8266_SendCmd(ESP8266_WIFI_INFO, "GOT IP"))
		DelayXms(500);
	
		Usart_Printf(USART_DEBUG, "5. CIPSTART\r\n");
		while(ESP8266_SendCmd(ESP8266_ONENET_INFO, "CONNECT"))
		DelayXms(500);
	
		Usart_Printf(USART_DEBUG, "6. ESP8266 Init OK\r\n");

}

//==========================================================
//	函数名称：	USART2_IRQHandler
//
//	函数功能：	串口2收发中断
//
//	入口参数：	无
//
//	返回参数：	无
//
//	说明：		使用环形缓冲区，防止溢出
//==========================================================
void USART2_IRQHandler(void)
{

	if(USART_GetITStatus(USART2, USART_IT_RXNE) != RESET) //接收中断
	{
		uint16_t nextHead;
		uint8_t data;
		
		data = USART2->DR;
		
		// 计算下一个写位置
		nextHead = (esp8266_rx.head + 1) % ESP8266_BUF_SIZE;
		
		// 检查是否溢出
		if (nextHead == esp8266_rx.tail)
		{
			// 缓冲区满，设置溢出标志，丢弃数据
			esp8266_rx.overflow = 1;
		}
		else
		{
			// 写入数据
			esp8266_buf[esp8266_rx.head] = data;
			esp8266_rx.head = nextHead;
			lastRxTick = Get_Tick();
		}
				
		USART_ClearFlag(USART2, USART_FLAG_RXNE);
	}

}
