#include "bsp_delay.h"

//系统滴答计数变量
volatile uint32_t sys_tick = 0;

//系统滴答定时器中断函数
void SysTick_Handler(void)
{
    sys_tick++;
}

/**
 * @brief  获取系统滴答计数
 * @retval 当前 tick 值 (ms)
 */
uint32_t Get_Tick(void)
{
    return sys_tick;
}

/**
 * @brief  初始化系统滴答定时器，固定为 1ms 中断一次
 * @note   在中断时间可能多多少少会有 1ms 误差
 */
void SysTick_Init(void)
{
    // SystemCoreClock / 1000 可以保证 1ms 中断的装载值
    if (SysTick_Config(SystemCoreClock / 1000) != 0)
    {
        // 配置失败，通常是因为时钟频率太高导致装载值超过 24 位
        // 在这里可以通过一些方式上报错误，比如死循环，或者点亮某个指示灯
        while(1);
    }
}


void DelayUs(uint32_t time)
{
	while(time--) {
		delay_1us();
	}
}

void DelayXms(uint32_t time)
{
	uint64_t t = time*1000;
	while(t--) {
		delay_1us();
	}
}



