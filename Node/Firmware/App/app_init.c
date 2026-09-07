/****************************************************************************
 * 应用层初始化函数实现 - app_init.c
 *
 * 功能描述:
 *   系统启动时统一执行的初始化流程, 封装硬件和业务初始化
 *   包括 BSP 外设初始化和 WiFi 连接初始化(最多3次重试)
 *
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "bsp_ultrasonic.h"
#include "bsp_qmc5883p.h"
#include "bsp_lora.h"
#include "node_config.h"
#include "app_global.h"

/****************************************************************************
 * 函数名: BSP_Init
 * 功能:   初始化所有板级支持包(BSP)设备
 * 参数:   无
 * 返回值: 无
 ****************************************************************************/
void BSP_Init(void)
{
    LED_Init();
    US_Init();
    SysTick_Init();
    Usart_Init();
    QMC5883P_Init(&qmc5883p, QMC5883P_MODE_CONTINUOUS, QMC5883P_ODR_100HZ, QMC5883P_RNG_8G);
    Config_Init();      /* 先读 Flash 配置区身份(地址/产品ID/设备名), 必须在 LoRa_Node_Init 之前 */
    LoRa_Node_Init();
    Usart_Printf(USART_DEBUG, "[SYS] 板级初始化完成\r\n");
}
