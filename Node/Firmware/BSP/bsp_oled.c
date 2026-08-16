#include "bsp_oled.h"
#include "bsp_oledfont.h"
#include "string.h"
#include <stdarg.h>
#include <stdio.h>  

unsigned int HZ=0;
u8 OLED_GRAM[144][8];
/*
*********************************************************************************************************
*	函 数 名: Oledfont_NUM
*	功能说明: 统计oledfont字库里汉字个数，使用OLED_ShowCH接口时需要使用
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
unsigned int Oledfont_NUM(void)
{
  unsigned int HZ_NUM;
  unsigned char *PT;
  PT = hz_index;
  while(*PT++ != '\0')
  {
  	 HZ_NUM++;
  }

  return HZ_NUM/2;
} 
/*
*********************************************************************************************************
*	函 数 名: IIC_GPIO_Config
*	功能说明: GPIO初始化函数
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void IIC_GPIO_Config(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	RCC_APB2PeriphClockCmd(OLED_SCL_RCC|OLED_SDA_RCC, ENABLE);	/* 开启GPIO时钟 */
	
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO ,ENABLE);//重映射需要使能AFIO时钟
  GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable,ENABLE);//只关闭JTAG，保留SWD
	
	GPIO_InitStructure.GPIO_Pin = OLED_SCL_PIN;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;  
	GPIO_Init(OLED_SCL_GPIO_PORT, &GPIO_InitStructure);
	
	GPIO_InitStructure.GPIO_Pin = OLED_SDA_PIN;
	GPIO_Init(OLED_SDA_GPIO_PORT, &GPIO_InitStructure);

	/* 给一个停止信号, 复位IIC总线上所有设备到待机模式 */
	IIC_Stop();
}
/*
*********************************************************************************************************
*	函 数 名: IIC_Delay
*	功能说明: 延时函数
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
static void IIC_Delay(void)
{
	uint8_t i;
	for (i = 0; i < 10; i++);
}

/*
*********************************************************************************************************
*	函 数 名: IIC_Start
*	功能说明: CPU发起IIC总线启动信号
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void IIC_Start(void)
{
	/* 当SCL高电平时，SDA出现一个下跳沿表示IIC总线启动信号 */
	OLED_SDA_H;
	OLED_SCL_H;
	IIC_Delay();
	OLED_SDA_L;
	IIC_Delay();
	OLED_SCL_L;
	IIC_Delay();
}

/*
*********************************************************************************************************
*	函 数 名: IIC_Stop
*	功能说明: CPU发起IIC总线停止信号
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void IIC_Stop(void)
{
	/* 当SCL高电平时，SDA出现一个上跳沿表示IIC总线停止信号 */
	OLED_SDA_L;
	OLED_SCL_H;
	IIC_Delay();
	OLED_SDA_H;
}

/*
*********************************************************************************************************
*	函 数 名: IIC_WaitAck
*	功能说明: CPU产生一个时钟，并读取器件的ACK应答信号
*	形    参：无
*	返 回 值: 返回0表示正确应答，1表示无器件响应
*********************************************************************************************************
*/
uint8_t IIC_WaitAck(void)
{
	uint8_t re;

	OLED_SDA_H;	/* CPU释放SDA总线 */
	IIC_Delay();
	OLED_SCL_H;	/* CPU驱动SCL = 1, 此时器件会返回ACK应答 */
	IIC_Delay();
	if (OLED_IIC_SDA_READ())	/* CPU读取SDA口线状态 */
	{
		re = 1;
	}
	else
	{
		re = 0;
	}
	OLED_SCL_L;
	IIC_Delay();
	return re;
}
/*
*********************************************************************************************************
*	函 数 名: Write_IIC_Byte
*	功能说明: 向IIC总线写一个字节
*	形    参：_ucByte
*	返 回 值: 无
*********************************************************************************************************
*/
void Write_IIC_Byte(uint8_t _ucByte)
{
 	uint8_t i;
	/* 先发送字节的高位bit7 */
	for (i = 0; i < 8; i++)
	{		
		if (_ucByte & 0x80)
		{
			OLED_SDA_H;
		}
		else
		{
			OLED_SDA_L;
		}
		IIC_Delay();
		OLED_SCL_H;
		IIC_Delay();	
		OLED_SCL_L;
		if (i == 7)
		{
			 OLED_SDA_H; // 释放总线
		}
		_ucByte <<= 1;	/* 左移一个bit */
		IIC_Delay();
	}
}
/*
*********************************************************************************************************
*	函 数 名: Write_IIC_Command
*	功能说明: 向OLED显示器写命令
*	形    参：IIC_Command
*	返 回 值: 无
*********************************************************************************************************
*/

void Write_IIC_Command(u8 IIC_Command)
{
	IIC_Start();
	Write_IIC_Byte(OLED_ADDRESS);//OLED地址
	IIC_WaitAck();
	Write_IIC_Byte(0x00);//寄存器地址
	IIC_WaitAck();
	Write_IIC_Byte(IIC_Command);
	IIC_WaitAck();
	IIC_Stop();
}

/*
*********************************************************************************************************
*	函 数 名: Write_IIC_Data
*	功能说明: 向OLED显示器写数据
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/

void Write_IIC_Data(u8 IIC_Data)
{
	IIC_Start();
	Write_IIC_Byte(OLED_ADDRESS);//OLED地址
	IIC_WaitAck();
	Write_IIC_Byte(0x40);//寄存器地址
	IIC_WaitAck();
	Write_IIC_Byte(IIC_Data);
	IIC_WaitAck();
	IIC_Stop();
}
/*
*********************************************************************************************************
*	函 数 名: OLED_Refresh
*	功能说明: 更新显存到OLED	
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_Refresh(void)
{
	u8 i,n;
	for(i=0;i<8;i++)
	{
	   Write_IIC_Command(0xb0+i); //设置页起始地址
	   Write_IIC_Command(0x00);   //设置低列起始地址
	   Write_IIC_Command(0x10);   //设置高列起始地址
	   for(n=0;n<128;n++)
		Write_IIC_Data(OLED_GRAM[n][i]);
  }
}
/*
*********************************************************************************************************
*	函 数 名: OLED_Set_Pos
*	功能说明: 设置起始坐标
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_Set_Pos(u8 x, u8 y) 
{ 
	Write_IIC_Command(0xb0+y);
	Write_IIC_Command((((x+2)&0xf0)>>4)|0x10);
	Write_IIC_Command(((x+2)&0x0f)|0x01);
}
/*
*********************************************************************************************************
*	函 数 名: OLED_ON
*	功能说明: 开OLED显示，将屏幕从黑屏变亮
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_ON(void)
{
	Write_IIC_Command(0X8D);  //设置电荷泵
	Write_IIC_Command(0X14);  //开启电荷泵
	Write_IIC_Command(0XAF);  //OLED唤醒
}
/*
*********************************************************************************************************
*	函 数 名: OLED_OFF
*	功能说明: 关OLED显示 -- 关显示模式,OLED功耗小于10uA
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_OFF(void)
{
	Write_IIC_Command(0X8D);  //设置电荷泵
	Write_IIC_Command(0X10);  //关闭电荷泵
	Write_IIC_Command(0XAE);  //OLED休眠
}

/*
*********************************************************************************************************
*	函 数 名: OLED_Fill
*	功能说明: OLED全屏填充
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_Fill(u8 fill_Data)
{
	u8 m,n;
	for(m=0;m<8;m++)
	{
		Write_IIC_Command(0xb0+m);		//page0-page1
		Write_IIC_Command(0x00);		//low column start address
		Write_IIC_Command(0x10);		//high column start address
		for(n=0;n<130;n++)
		{
			Write_IIC_Data(fill_Data);
		}
	}
}

/*
*********************************************************************************************************
*	函 数 名: OLED_Clear
*	功能说明: 清屏函数
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_Clear(void)
{
	OLED_Fill(0x00);
}
/*
*********************************************************************************************************
*	函 数 名: OLED_ShowChar
*	功能说明: 在指定位置显示一个字符
*	形    参：OLED显示的坐标位置。x:0~127 y:0~63
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_ShowChar(u8 x, u8 y, u8 chr)
{
  u8 c = 0, i = 0;
  c = chr - ' '; //得到偏移后的值
  if(x > 130 - 1)
  {
    x = 0;
    y = y + 2;
  }
	OLED_Set_Pos(x, y);
	for(i = 0; i < 8; i++)
		Write_IIC_Data(zf[c * 16 + i]);
	OLED_Set_Pos(x, y + 1);
	for(i = 0; i < 8; i++)
		Write_IIC_Data(zf[c * 16 + i + 8]);

}
/*
*********************************************************************************************************
*	函 数 名: oled_pow
*	功能说明: m^n函数
*	形    参：m,n
*	返 回 值: 无
*********************************************************************************************************
*/

u32 oled_pow(u8 m, u8 n)
{
  u32 result = 1;
  while(n--)result *= m;
  return result;
}

/*
*********************************************************************************************************
*	函 数 名: OLED_ShowNum
*	功能说明: 在指定位置显示数字
*	形    参：x:0~127 y:0~63 
					 len 数值的位数 
					 num 数值(0~4294967295) 
					 mode 1:显示0  0:显示空格
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_ShowNum(u8 x, u8 y, u32 num, u8 len,u8 mode)
{
  u8 t, temp;
  for(t = 0; t < len; t++)
  {
    temp = (num / oled_pow(10, len - t - 1)) % 10;
		if(temp == 0)
		{
			if(mode)
				OLED_ShowChar(x + 8*t, y, '0');
			else
				OLED_ShowChar(x + 8*t, y, ' ');
			continue;
		}
		else
			OLED_ShowChar(x + 8*t, y, temp + '0');
  }
}
/*
*********************************************************************************************************
*	函 数 名: OLED_ShowCH
*	功能说明: 在指定位置显示中英文字符
*	形    参：OLED显示的坐标位置。x:0~127 y:0~63
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_ShowCH(u8 x, u8 y,u8 *chs)
{
  u32 i=0;
	u32 j;
	char* m;
	while (*chs != '\0')
	{
		if (*chs > 0xa0)				//汉字编码都是大于0xa0
		{
			for (i=0 ;i < HZ;i++)
			{	
				if(x>112)
				{
					x=0;
					y=y+2;
				}
				if ((*chs == hz_index[i*3]) && (*(chs+1) == hz_index[i*3+1]))
				{
					OLED_Set_Pos(x, y);
					for(j=0;j<16;j++)
						Write_IIC_Data(hz[i*32+j]);
					OLED_Set_Pos(x,y+1);
					for(j=0;j<16;j++)
						Write_IIC_Data(hz[i*32+j+16]);
					x +=16;
					break;
				}
			}
			chs+=2;
		}
		else
		{
			if(x>122)
			{
				x=0;
				y=y+2;
			}
			m=strchr(zf_index,*chs);
			if (m!=NULL)
			{
				OLED_Set_Pos(x, y);
				for(j = 0; j < 8; j++)
					Write_IIC_Data(zf[((u8)*m-' ') * 16 + j]);
				OLED_Set_Pos(x, y + 1);
				for(j = 0; j < 8; j++)
					Write_IIC_Data(zf[((u8)*m-' ') * 16 + j + 8]);
				x += 8;
			}
			chs++;
		}
	}
}


void OLED_Printf(u8 x,u8 y, const char *fmt, ...)
{
	va_list args;
	char buffer[50];

	va_start(args, fmt);           // 初始化可变参数列表，指向第一个可变参数
	vsprintf(buffer, fmt, args);   // 将格式化字符串和参数转换成普通字符串
	va_end(args);                  // 结束可变参数列表


	OLED_ShowCH(x, y, (u8 *)buffer);
}
/*
*********************************************************************************************************
*	函 数 名: OLED_Init
*	功能说明: OELD初始化
*	形    参：无
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_Init(void)
{
	IIC_GPIO_Config();

  Write_IIC_Command(0xAE); //--display off
  Write_IIC_Command(0x00); //---set low column address
  Write_IIC_Command(0x10); //---set high column address
  Write_IIC_Command(0x40); //--set start line address
  Write_IIC_Command(0xB0); //--set page address
  Write_IIC_Command(0x81); // contract control
  Write_IIC_Command(0xFF); //--128
  Write_IIC_Command(0xA1); //set segment remap
  Write_IIC_Command(0xA6); //--normal / reverse
  Write_IIC_Command(0xA8); //--set multiplex ratio(1 to 64)
  Write_IIC_Command(0x3F); //--1/32 duty
  Write_IIC_Command(0xC8); //Com scan direction
  Write_IIC_Command(0xD3); //-set display offset
  Write_IIC_Command(0x00); //
  Write_IIC_Command(0xD5); //set osc division
  Write_IIC_Command(0x80); //
  Write_IIC_Command(0xD8); //set area color mode off
  Write_IIC_Command(0x05); //
  Write_IIC_Command(0xD9); //Set Pre-Charge Period
  Write_IIC_Command(0xF1); //
  Write_IIC_Command(0xDA); //set com pin configuartion
  Write_IIC_Command(0x12); //
  Write_IIC_Command(0xDB); //set Vcomh
  Write_IIC_Command(0x30); //
  Write_IIC_Command(0x8D); //set charge pump enable
  Write_IIC_Command(0x14); //
  Write_IIC_Command(0xAF); //--turn on oled panel
		
	HZ = Oledfont_NUM();
	OLED_Clear();
}
/*
*********************************************************************************************************
*	函 数 名: OLED_DrawBMP
*	功能说明: 显示一个BMP图片
*	形    参：起始坐标(左上角) (x0:0~127, y0:0~7)
					  结束坐标(右下角) (x1:1~128, y1:1~8)
*	返 回 值: 无
*********************************************************************************************************
*/
void OLED_DrawBMP(u8 x0,u8 y0,u8 x1,u8 y1,u8 BMP[])
{
	u16 j=0;
	u8 x,y;

  if(y1%8==0)
		y = y1/8;
  else
		y = y1/8 + 1;
	for(y=y0;y<y1;y++)
	{
		OLED_Set_Pos(x0,y);
    for(x=x0;x<x1;x++)
		{
			Write_IIC_Data(BMP[j++]);
		}
	}
}
