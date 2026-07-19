/****************************************************************************
 * ADC驱动头文件 - bsp_adc.h
 * 
 * 功能描述:
 *   提供ADC模拟信号采集功能
 *   使用DMA方式连续采集，提高效率
 * 
 * 硬件配置:
 *   - ADC1: PB1(ADC_Channel_9)
 *   - DMA1_Channel1: 用于ADC数据传输
 *   - 转换通道数: 1
 * 
 * 注意:
 *   - 用作ADC采集的IO必须没有复用，否则采集电压会有影响
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/
#ifndef _BSP_ADC_H
#define _BSP_ADC_H

#include "stm32f10x.h"

/* ADC时钟配置 */
#define    ADC_APBxClock_FUN             RCC_APB2PeriphClockCmd
#define    ADC_CLK                       RCC_APB2Periph_ADC1

/* ADC GPIO配置 */
#define    ADC_GPIO_APBxClock_FUN        RCC_APB2PeriphClockCmd
#define    ADC_GPIO_CLK                  RCC_APB2Periph_GPIOB  
#define    ADC_PORT                      GPIOB

/* 注意：PC0在部分开发板上有特殊用途，做ADC转换时结果可能有误差 */

/* 转换通道个数 */
#define    NOFCHANEL                     1

/* ADC输入通道1配置 */
#define    ADC_PIN1                      GPIO_Pin_1
#define    ADC_CHANNEL1                  ADC_Channel_9

/* DMA配置 */
/* ADC1对应DMA1通道1，ADC3对应DMA2通道5，ADC2没有DMA功能 */
#define    ADC_x                         ADC1
#define    ADC_DMA_CHANNEL               DMA1_Channel1
#define    ADC_DMA_CLK                   RCC_AHBPeriph_DMA1

/****************************************************************************
 * 函数名: ADCx_Init
 * 功能:   初始化ADC及DMA
 * 参数:   无
 * 返回值: 无
 ****************************************************************************/
void ADCx_Init(void);

#endif /* __ADC_H */