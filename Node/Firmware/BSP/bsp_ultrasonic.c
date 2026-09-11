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
 *   - PB15(Trig): 触发信号输出(推挽输出)
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
#include "bsp_usart.h"

/* ==================== 非阻塞测量状态机 ==================== */
typedef enum
{
    US_ST_IDLE = 0,     /* 空闲: 无测量进行 */
    US_ST_WAIT_RISE,    /* 已发Trig, 等回波上升沿(TIM1_CC1) */
    US_ST_WAIT_FALL     /* 已捕获上升沿, 等回波下降沿(TIM1_CC2) */
} us_state_t;

static us_state_t s_state     = US_ST_IDLE;
static uint16_t   s_riseTicks = 0;   /* 上升沿时刻(TIM1捕获值, 单位µs) */
static uint32_t   s_phaseTick = 0;   /* 当前阶段的起点(ms), 用于超时判定 */



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
    RCC_APB2PeriphClockCmd(US_TRIG_CLK, ENABLE);

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
 * 函数名: US_StartMeasure
 * 功能:   启动一次超声波测量(非阻塞)
 * 参数:   无
 * 返回值: 1=已启动; 0=上一次测量尚未结束, 本次跳过(不重入)
 * 说明:   复位计数 -> 清捕获标志 -> 开TIM1 -> 发15µs触发脉冲,
 *         随后立即返回, 由 US_Poll() 在后续主循环中推进
 ****************************************************************************/
uint8_t US_StartMeasure(void)
{
    /* 上一次测量还没结束: 不重入, 等它自己完成或超时 */
    if (s_state != US_ST_IDLE)
        return 0;

    TIM_SetCounter(TIM1, 0);
    TIM_ClearFlag(TIM1, TIM_FLAG_CC1 | TIM_FLAG_CC2);
    TIM_Cmd(TIM1, ENABLE);

    /* 15µs 触发脉冲 */
    GPIO_SetBits(US_TRIG_PORT, US_TRIG_PIN);
    DelayXus(15);
    GPIO_ResetBits(US_TRIG_PORT, US_TRIG_PIN);

    s_phaseTick = Get_Tick();
    s_state     = US_ST_WAIT_RISE;
    return 1;
}

/****************************************************************************
 * 函数名: US_Poll
 * 功能:   轮询推进测量状态机(非阻塞)
 * 参数:   outDistCm - 测量完成时写入原始距离(cm)
 * 返回值: US_MEAS_NONE / US_MEAS_DONE / US_MEAS_TIMEOUT
 * 说明:   先判捕获标志(硬件已锁存, 晚读不丢), 后判超时;
 *         两次捕获值因启动时复位计数, 正常测量不会跨 65.5ms 回绕
 ****************************************************************************/
uint8_t US_Poll(uint16_t *outDistCm)
{
    switch (s_state)
    {
    case US_ST_WAIT_RISE:
        if (TIM_GetFlagStatus(TIM1, TIM_FLAG_CC1) != RESET)
        {
            TIM_ClearFlag(TIM1, TIM_FLAG_CC1);
            s_riseTicks = TIM_GetCapture1(TIM1);   /* 上升沿时刻 */
            s_phaseTick = Get_Tick();              /* 下降沿阶段重新计时 */
            s_state     = US_ST_WAIT_FALL;
        }
        else if ((Get_Tick() - s_phaseTick) >= US_ECHO_TIMEOUT_MS)
        {
            TIM_Cmd(TIM1, DISABLE);
            s_state = US_ST_IDLE;
            Usart_Printf(USART_DEBUG, "US: CC1 timeout (no echo start)\r\n");
            return US_MEAS_TIMEOUT;
        }
        break;

    case US_ST_WAIT_FALL:
        if (TIM_GetFlagStatus(TIM1, TIM_FLAG_CC2) != RESET)
        {
            uint16_t fallTicks = TIM_GetCapture2(TIM1);   /* 下降沿时刻 */
            TIM_ClearFlag(TIM1, TIM_FLAG_CC2);
            TIM_Cmd(TIM1, DISABLE);
            s_state = US_ST_IDLE;

            /* 回波脉宽(µs) -> 距离(cm):
             * distance = 0.5 * 声速(340m/s) * t, t 单位µs, 1e-4 换算 */
            if (outDistCm != 0)
                *outDistCm = (uint16_t)(0.5f * 340.0f
                                        * (float)(uint16_t)(fallTicks - s_riseTicks) * 1e-4f);
            return US_MEAS_DONE;
        }
        else if ((Get_Tick() - s_phaseTick) >= US_ECHO_TIMEOUT_MS)
        {
            TIM_Cmd(TIM1, DISABLE);
            s_state = US_ST_IDLE;
            Usart_Printf(USART_DEBUG, "US: CC2 timeout (no echo end)\r\n");
            return US_MEAS_TIMEOUT;
        }
        break;

    default:
        break;
    }

    return US_MEAS_NONE;
}
