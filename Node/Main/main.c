/****************************************************************************
 * 智能停车场系统 - 主程序入口
 *
 * 功能描述:
 *   基于STM32 + ESP8266实现的智能停车场车位检测系统
 *
 * 架构说明:
 *   main.c 仅作为程序入口，所有业务逻辑拆分到 App/ 层：
 *     - App/app_init.c     初始化函数 (BSP_Init, Wifi_Init)
 *     - App/app_tasks.c    周期任务函数 + 全局变量定义
 *     - App/app_global.h   全局变量声明
 *
 * 主循环流程(时间戳非阻塞架构):
 *   1. 超声波数据更新
 *   2. 地磁数据采集
 *   3. 车位状态检测
 *   4. LED控制
 *   5. WiFi通信: 数据上报 + 下行指令处理
 *
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#include "stm32f10x.h"
#include "app_global.h"
#include "app_init.h"
#include "app_tasks.h"
#include "bsp_usart.h"
#include "bsp_delay.h"

/* 固件版本: 节点端 LoRa 网关方案 (v2.1) */
#define NODE_FW_VERSION     "v2.1"

/* ==================== 看门狗 ==================== */
/* 独立看门狗 IWDG: LSI 时钟 40kHz, 64 分频 -> 625Hz(1.6ms/计数值),
 * 重装载 2500 -> 超时约 4s. 主循环每轮完成即喂狗,
 * 任何任务卡死超过 4s 未喂狗, 芯片自动复位 */
#define IWDG_RELOAD_VALUE   2500

/* 喂狗日志打印间隔(ms): 每 10 秒打一次, 避免每轮刷屏淹没其他日志 */
#define WDG_LOG_INTERVAL_MS 10000

static void WDG_Init(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);   /* 解除写保护 */
    IWDG_SetPrescaler(IWDG_Prescaler_64);           /* 分频 64 */
    IWDG_SetReload(IWDG_RELOAD_VALUE);              /* 超时约 4s */
    IWDG_ReloadCounter();                           /* 先装载再使能 */
    IWDG_Enable();                                  /* 启动看门狗 */
}

/* 喂狗 + 周期打印状态(便于调试):
 * 复位后运行时长从 0 重新计时, 可据此判断芯片是否发生过看门狗复位 */
static void WDG_Feed(void)
{
    static uint32_t s_lastDbgTick = 0;

    IWDG_ReloadCounter();                           /* 喂狗 */

    if (Get_Tick() - s_lastDbgTick >= WDG_LOG_INTERVAL_MS)
    {
        s_lastDbgTick = Get_Tick();
        Usart_Printf(USART_DEBUG, "[WDG] 喂狗正常, 运行 %lu 秒\r\n",
                     (unsigned long)(Get_Tick() / 1000));
    }
}

int main(void)
{
    /* 初始化所有板级外设(含LoRa模块) */
    BSP_Init();

    /* 启动看门狗: 此后主循环必须周期性喂狗 */
    WDG_Init();

    /* 上电横幅: 区分当前烧录的是节点端/网关端固件 */
    Usart_Printf(USART_DEBUG,
        "\r\n[SYS] ========== 智能停车场节点 %s ==========\r\n", NODE_FW_VERSION);

    /* 主循环 - 时间戳非阻塞架构 */
    while (1)
    {
		
        US_Task();                  /* 超声波采样 */
        ParkingStatus_Check();      /* 车位状态检测 */
        QMC_Task();                 /* 地磁采集 */
        LED_Task();                 /* LED控制 */
        LoRa_Task();                /* LoRa通信: 响应网关轮询 + 下行命令 */

        /* 喂狗(每10秒打印一次状态): 任一段代码卡死超4s自动复位 */
        WDG_Feed();
    }
}
