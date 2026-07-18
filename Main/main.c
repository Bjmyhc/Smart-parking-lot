#include <stdio.h>
#include <stdlib.h>
#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "esp8266.h"
#include "onenet.h"

#define UPLOAD_INTERVAL 5000

char PublishBuf[256];

const char PubTopic[] = "$sys/53BV12EYcY/park1/thing/property/post";

const char *SubTopic[] = {"$sys/53BV12EYcY/park1/thing/property/set"};

unsigned char *pData = NULL;

void BSP_Init(void)
{
    LED_Init();
    SysTick_Init();
    Usart_Init();
    Usart_Printf(USART1, "USART Init OK!\n");
}

void GenerateParkingData(void)
{
    sprintf(PublishBuf,
            "{\"id\":\"%u\",\"params\":{"
            "\"ParkStatus\":{\"value\":%d},"
            "\"GeoMagnetic\":{\"value\":%d},"
            "\"Ultrasonic\":{\"value\":%d},"
            "\"OccupiedTime\":{\"value\":%d}}}",
            (unsigned int)Get_Tick(),
            0,
            0,
            0,
            0);
}

int main(void)
{
    uint32_t LastUploadTick = 0;

    BSP_Init();

    Usart_Printf(USART_DEBUG, "ESP8266 Init...\n");
    ESP8266_Init();

    if (OneNet_DevLink() == ERROR)
    {
        DelayXms(500);
    }

    Usart_Printf(USART_DEBUG, "OneNET Connected!\n");

    OneNet_Subscribe(SubTopic, 1);

    while (1)
    {
        if (Get_Tick() - LastUploadTick >= UPLOAD_INTERVAL)
        {
            GenerateParkingData();
            OneNet_Publish(PubTopic, PublishBuf);
            ESP8266_Clear();
            LastUploadTick = Get_Tick();
        }

        pData = ESP8266_GetIPD(20);

        if (pData != NULL)
        {
            OneNet_RevPro(pData);
            ESP8266_Clear();
        }
    }
}