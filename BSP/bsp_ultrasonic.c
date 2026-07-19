/****************************************************************************
 * 超声波传感器驱动 - bsp_ultrasonic.c
 * 
 * 功能描述:
 *   实现超声波传感器(HC-SR04)的数据采集
 *   使用TIM1输入捕获测量回波信号脉宽
 *   计算距离并返回结果(单位:cm)
 * 
 * 硬件配置:
 *   - TIM1定时器: 72MHz系统时钟, 输入捕获模式
 *   - PA0(Trig): 触发信号输出(推挽输出)
 *   - PA8(Echo): 回波信号输入(浮空输入)
 *   - 通道1捕获上升沿,通道2捕获下降沿
 * 
 * 工作原理:
 *   1. 发送15us触发脉冲
 *   2. 等待Echo引脚上升沿(开始计时)
 *   3. 等待Echo引脚下降沿(结束计时)
 *   4. 计算脉宽时间
 *   5. 根据公式计算距离: 距离 = 0.5 * 声速(340m/s) * 时间
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "bsp_ultrasonic.h"
#include "bsp_delay.h"



/****************************************************************************
 * 函数名: US_Init
 * 功能:   初始化超声波传感器
 * 参数:   无
 * 返回值: 无
 * 说明:   配置TIM1输入捕获、Trig和Echo引脚
 ****************************************************************************/
void US_Init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);

    TIM_TimeBaseInitTypeDef TIM_BaseInitStruct;
    TIM_TimeBaseStructInit(&TIM_BaseInitStruct);
    TIM_BaseInitStruct.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_BaseInitStruct.TIM_Period = 65536 - 1;
    TIM_BaseInitStruct.TIM_Prescaler = 72 - 1;
    TIM_BaseInitStruct.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM1, &TIM_BaseInitStruct);

    RCC_APB2PeriphClockCmd(US_ECHO_CLK, ENABLE);

    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStruct.GPIO_Pin = US_ECHO_PIN;
    GPIO_Init(US_ECHO_PORT, &GPIO_InitStruct);

    TIM_ICInitTypeDef IC_InitStruct;
    TIM_ICStructInit(&IC_InitStruct);

    IC_InitStruct.TIM_Channel = TIM_Channel_1;
    IC_InitStruct.TIM_ICFilter = 0;
    IC_InitStruct.TIM_ICPolarity = TIM_ICPolarity_Rising;
    IC_InitStruct.TIM_ICPrescaler = TIM_ICPSC_DIV1;
    IC_InitStruct.TIM_ICSelection = TIM_ICSelection_DirectTI;
    TIM_ICInit(TIM1, &IC_InitStruct);

    IC_InitStruct.TIM_Channel = TIM_Channel_2;
    IC_InitStruct.TIM_ICFilter = 0;
    IC_InitStruct.TIM_ICPolarity = TIM_ICPolarity_Falling;
    IC_InitStruct.TIM_ICPrescaler = TIM_ICPSC_DIV1;
    IC_InitStruct.TIM_ICSelection = TIM_ICSelection_IndirectTI;
    TIM_ICInit(TIM1, &IC_InitStruct);

    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStruct.GPIO_Pin = US_TRIG_PIN;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_10MHz;
    GPIO_Init(US_TRIG_PORT, &GPIO_InitStruct);
}

/****************************************************************************
 * 函数名: US_GetDistance
 * 功能:   获取超声波测量距离
 * 参数:   无
 * 返回值: 距离值(单位:cm)
 * 说明:   发送触发脉冲,测量回波脉宽,计算距离
 *         公式: distance = 0.5 * 340 * GoBackTime * 1e-4
 ****************************************************************************/
uint16_t US_GetDistance(void)
{
    TIM_SetCounter(TIM1, 0);
    TIM_ClearFlag(TIM1, TIM_FLAG_CC1);
    TIM_ClearFlag(TIM1, TIM_FLAG_CC2);

    TIM_Cmd(TIM1, ENABLE);

    GPIO_SetBits(US_TRIG_PORT, US_TRIG_PIN);
    DelayXus(15);
    GPIO_ResetBits(US_TRIG_PORT, US_TRIG_PIN);

    uint32_t startTime = Get_Tick();
    while (TIM_GetFlagStatus(TIM1, TIM_FLAG_CC1) == RESET)
    {
        if (Get_Tick() - startTime >= 200)
        {
            TIM_Cmd(TIM1, DISABLE);
            return 0;
        }
    }

    startTime = Get_Tick();
    while (TIM_GetFlagStatus(TIM1, TIM_FLAG_CC2) == RESET)
    {
        if (Get_Tick() - startTime >= 200)
        {
            TIM_Cmd(TIM1, DISABLE);
            return 0;
        }
    }

    TIM_Cmd(TIM1, DISABLE);

    uint16_t GoBackTime = TIM_GetCapture2(TIM1) - TIM_GetCapture1(TIM1);

    float distance = 0.5f * 340.0f * GoBackTime * 1e-4f;

    return (uint16_t)distance;
}
