/****************************************************************************
 * 定时器驱动 - bsp_timer.c
 * 
 * 功能描述:
 *   实现TIM4定时器的初始化配置和中断处理
 *   用于系统周期性任务调度、定时计数等
 * 
 * 硬件配置:
 *   - TIM4定时器: 72MHz系统时钟, 1ms中断周期
 *   - 预分频器: 71, 自动重装载值: 1000
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "bsp_timer.h"

/****************************************************************************
 * 函数名: GENERAL_TIM_NVIC_Config
 * 功能:   配置TIM4定时器中断优先级
 * 参数:   无
 * 返回值: 无
 * 说明:   设置抢占优先级为1,子优先级为2
 ****************************************************************************/
static void GENERAL_TIM_NVIC_Config(void)
{
    NVIC_InitTypeDef NVIC_InitStructure;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_0);

    NVIC_InitStructure.NVIC_IRQChannel = GENERAL_TIM_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

/****************************************************************************
 * 函数名: GENERAL_TIM_Mode_Config
 * 功能:   配置TIM4定时器工作模式
 * 参数:   无
 * 返回值: 无
 * 说明:   配置定时器时钟、预分频器、自动重装载值等
 *         中断周期 = (Prescaler+1) * (Period+1) / 72MHz = 1ms
 ****************************************************************************/
static void GENERAL_TIM_Mode_Config(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;

    GENERAL_TIM_APBxClock_FUN(GENERAL_TIM_CLK, ENABLE);

    TIM_TimeBaseStructure.TIM_Period = GENERAL_TIM_Period;
    TIM_TimeBaseStructure.TIM_Prescaler = GENERAL_TIM_Prescaler;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;

    TIM_TimeBaseInit(GENERAL_TIM, &TIM_TimeBaseStructure);

    TIM_ClearFlag(GENERAL_TIM, TIM_FLAG_Update);

    TIM_ITConfig(GENERAL_TIM, TIM_IT_Update, ENABLE);

    TIM_Cmd(GENERAL_TIM, ENABLE);
}

/****************************************************************************
 * 函数名: GENERAL_TIM_Init
 * 功能:   初始化TIM4通用定时器
 * 参数:   无
 * 返回值: 无
 * 说明:   包含中断配置和模式配置
 ****************************************************************************/
void GENERAL_TIM_Init(void)
{
    GENERAL_TIM_NVIC_Config();
    GENERAL_TIM_Mode_Config();
}

/****************************************************************************
 * 函数名: GENERAL_TIM_IRQHandler
 * 功能:   TIM4定时器中断服务函数
 * 参数:   无
 * 返回值: 无
 * 说明:   处理定时器更新中断,清除中断标志位
 ****************************************************************************/
void GENERAL_TIM_IRQHandler(void)
{
    if (TIM_GetITStatus(GENERAL_TIM, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(GENERAL_TIM, TIM_FLAG_Update);
    }
}