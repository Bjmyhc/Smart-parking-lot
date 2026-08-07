/****************************************************************************
 * 定时器驱动头文件 - bsp_timer.h
 * 
 * 功能描述:
 *   提供通用定时器TIM4的初始化功能
 *   用于产生定时中断，实现周期性任务调度
 * 
 * 硬件配置:
 *   - TIM4: 72MHz时钟，1ms中断周期
 *   - 自动重载值: 1000
 *   - 预分频系数: 71
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/
#ifndef BSP_TIMER_H
#define BSP_TIMER_H

#include "stm32f10x.h"

/* TIM4定时器配置 */
#define            GENERAL_TIM                   TIM4
#define            GENERAL_TIM_APBxClock_FUN     RCC_APB1PeriphClockCmd
#define            GENERAL_TIM_CLK               RCC_APB1Periph_TIM4
#define            GENERAL_TIM_Period            1000      /* 自动重载值 */
#define            GENERAL_TIM_Prescaler         71        /* 预分频系数 */
#define            GENERAL_TIM_IRQ               TIM4_IRQn
#define            GENERAL_TIM_IRQHandler        TIM4_IRQHandler

/****************************************************************************
 * 函数名: GENERAL_TIM_Init
 * 功能:   初始化通用定时器TIM4
 * 参数:   无
 * 返回值: 无
 * 说明:   定时器频率 = 72MHz / (71+1) = 1MHz
 *         中断周期 = 1000 / 1MHz = 1ms
 ****************************************************************************/
void GENERAL_TIM_Init(void);

#endif
