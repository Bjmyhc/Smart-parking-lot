#ifndef QMC5883L_H
#define QMC5883L_H

#include "stm32f10x.h"

void QMC_Init(void);                              // 初始化地磁
void QMC_ReadRaw(int16_t *x, int16_t *y, int16_t *z);  // 读原始 xyz

#endif