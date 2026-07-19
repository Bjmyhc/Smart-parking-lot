/****************************************************************************
 * ADC驱动 - bsp_adc.c
 * 
 * 功能描述:
 *   实现ADC模数转换器的初始化配置和数据采集
 *   使用DMA方式自动传输转换结果
 *   用于地磁传感器等模拟信号采集
 * 
 * 硬件配置:
 *   - ADC1: 独立模式, 连续转换, 扫描模式
 *   - 通道1(PA1): 用于地磁传感器数据采集
 *   - DMA通道: 自动传输转换结果到内存
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "bsp_adc.h"

/* ADC转换结果缓冲区 */
uint16_t ADC_ConvertedValue[NOFCHANEL] = {0};

/****************************************************************************
 * 函数名: ADCx_GPIO_Config
 * 功能:   配置ADC引脚
 * 参数:   无
 * 返回值: 无
 * 说明:   配置ADC输入引脚为模拟输入模式
 ****************************************************************************/
static void ADCx_GPIO_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    ADC_GPIO_APBxClock_FUN(ADC_GPIO_CLK, ENABLE);

    GPIO_InitStructure.GPIO_Pin = ADC_PIN1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;

    GPIO_Init(ADC_PORT, &GPIO_InitStructure);
}

/****************************************************************************
 * 函数名: ADCx_Mode_Config
 * 功能:   配置ADC工作模式
 * 参数:   无
 * 返回值: 无
 * 说明:   配置DMA传输、ADC转换模式、通道配置等
 ****************************************************************************/
static void ADCx_Mode_Config(void)
{
    DMA_InitTypeDef DMA_InitStructure;
    ADC_InitTypeDef ADC_InitStructure;

    RCC_AHBPeriphClockCmd(ADC_DMA_CLK, ENABLE);
    ADC_APBxClock_FUN(ADC_CLK, ENABLE);

    DMA_DeInit(ADC_DMA_CHANNEL);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)(&(ADC_x->DR));
    DMA_InitStructure.DMA_MemoryBaseAddr = (u32)ADC_ConvertedValue;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize = NOFCHANEL;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;

    DMA_Init(ADC_DMA_CHANNEL, &DMA_InitStructure);

    DMA_Cmd(ADC_DMA_CHANNEL, ENABLE);

    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = ENABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = ENABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel = NOFCHANEL;

    ADC_Init(ADC_x, &ADC_InitStructure);

    ADC_RegularChannelConfig(ADC_x, ADC_CHANNEL1, 1, ADC_SampleTime_55Cycles5);

    ADC_DMACmd(ADC_x, ENABLE);

    ADC_Cmd(ADC_x, ENABLE);

    ADC_ResetCalibration(ADC_x);
    while (ADC_GetResetCalibrationStatus(ADC_x));

    ADC_StartCalibration(ADC_x);
    while (ADC_GetCalibrationStatus(ADC_x));

    ADC_SoftwareStartConvCmd(ADC_x, ENABLE);
}

/****************************************************************************
 * 函数名: ADCx_Init
 * 功能:   初始化ADC模块
 * 参数:   无
 * 返回值: 无
 * 说明:   包含引脚配置和模式配置
 ****************************************************************************/
void ADCx_Init(void)
{
    ADCx_GPIO_Config();
    ADCx_Mode_Config();
}