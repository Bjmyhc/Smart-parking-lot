#ifndef ULTRASONIC_H
#define ULTRASONIC_H
#include "stm32f10x.h"

void US_Init(void);                // 初始化超声波
uint16_t US_GetDistance(void);     // 读距离，单位 cm

#endif