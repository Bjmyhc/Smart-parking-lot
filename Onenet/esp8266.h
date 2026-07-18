#ifndef _ESP8266_H_
#define _ESP8266_H_





#define REV_OK		0	//接收完成标志
#define REV_WAIT	1	//接收未完成标志

/* 环形缓冲区大小 */
#define ESP8266_BUF_SIZE	512

/* 接收状态标志 */
typedef struct {
    volatile uint16_t head;     // 写指针（中断写入）
    volatile uint16_t tail;     // 读指针（主循环读取）
    volatile uint8_t overflow;  // 溢出标志
} ESP8266_RxBuffer_t;


void ESP8266_Init(void);

void ESP8266_Clear(void);

_Bool ESP8266_SendCmd(char *cmd, char *res);

void ESP8266_SendData(unsigned char *data, unsigned short len);

unsigned char *ESP8266_GetIPD(unsigned short timeOut);

/* 获取缓冲区数据长度 */
uint16_t ESP8266_GetDataLen(void);

/* 检查是否溢出 */
uint8_t ESP8266_IsOverflow(void);

/* 清除溢出标志 */
void ESP8266_ClearOverflow(void);


#endif
