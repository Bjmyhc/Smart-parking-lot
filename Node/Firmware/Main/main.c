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
#include "app_version.h"    /* 版本单一源头: NODE_FW_VERSION */
#include "bsp_usart.h"
#include "bsp_delay.h"
#include "bsp_oled.h"

/* ==================== 看门狗 ==================== */
/* 独立看门狗 IWDG: LSI 时钟 40kHz, 64 分频 -> 625Hz(1.6ms/计数值),
 * 重装载 2500 -> 超时约 4s. 主循环每轮完成即喂狗,
 * 任何任务卡死超过 4s 未喂狗, 芯片自动复位 */
#define IWDG_RELOAD_VALUE   2500

/* 喂狗日志打印间隔(ms): 每 10 秒打一次, 避免每轮刷屏淹没其他日志 */
#define WDG_LOG_INTERVAL_MS 10000

static void WDG_Init(void)
{
    /* ⭐ 关键修复1: 先开启 LSI 时钟 (IWDG 唯一时钟源), 并等待稳定
     * 不开 LSI 直接操作 IWDG = 看门狗完全失效, 卡死永远不复位 */
    RCC_LSICmd(ENABLE);
    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);

    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);   /* 解除写保护 */
    IWDG_SetPrescaler(IWDG_Prescaler_64);           /* 分频 64 */
    /* ⭐ 关键修复2: 等待分频值写入完成 (PVU=0), 否则写操作被硬件丢弃 */
    while (IWDG_GetFlagStatus(IWDG_FLAG_PVU) != RESET);

    IWDG_SetReload(IWDG_RELOAD_VALUE);              /* 超时约 4s */
    /* ⭐ 关键修复3: 等待重载值写入完成 (RVU=0) */
    while (IWDG_GetFlagStatus(IWDG_FLAG_RVU) != RESET);

    IWDG_ReloadCounter();                           /* 先装载再使能 */
    IWDG_Enable();                                  /* 启动看门狗 */
    Usart_Printf(USART_DEBUG, "[WDG] 初始化完成: LSI已开启, 超时≈4s\r\n");
}

/* 喂狗: 仅执行看门狗重载操作 (纯原子, 无打印/判断, 100% 不会卡死).
 * 任何任务卡死导致主循环没跑到本行 → 4s 后门狗自动复位 */
static void WDG_Feed(void)
{
    IWDG_ReloadCounter();
}

/****************************************************************************
 * Print reset reason at boot: POR/PDR = power-drop restart,
 * IWDG = watchdog-stuck restart, NRST = button/flasher reset,
 * SW = software/OTA reset. Flags are read then cleared so the next
 * reset reason stays distinguishable (no accumulation).
 ****************************************************************************/
static void PrintResetReason(void)
{
    uint8_t any = 0;
    Usart_Printf(USART_DEBUG, "[BOOT] reset cause:");
    if (RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET)
    { Usart_Printf(USART_DEBUG, " POR/PDR(power drop)"); any = 1; }
    if (RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET)
    { Usart_Printf(USART_DEBUG, " NRST(button/flasher)"); any = 1; }
    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET)
    { Usart_Printf(USART_DEBUG, " IWDG(watchdog stuck)"); any = 1; }
    if (RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET)
    { Usart_Printf(USART_DEBUG, " WWDG(window wdt)"); any = 1; }
    if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET)
    { Usart_Printf(USART_DEBUG, " SW(software/OTA)"); any = 1; }
    if (!any) Usart_Printf(USART_DEBUG, " none/unknown");
    Usart_Printf(USART_DEBUG, "\r\n");
    RCC_ClearFlag();   /* clear flags so next reset cause is independent */
}

int main(void)
{
    /* 设置向量表偏移: OTA 后 APP 从 0x08004000 启动 */
    SCB->VTOR = FLASH_BASE | 0x4000;

    /* ⭐ 中断优先级分组: 2 位抢占 + 2 位子优先级(Group_2, 工程惯例).
     * 必须在任何 NVIC_Init 之前调用一次且仅一次, 否则默认 Group_0
     * (0 抢占位), PreemptionPriority 字段被忽略, 所有中断不能互相抢占.
     * 现有配置: USART2(LoRa收) Pre=0 Sub=0, USART1(Debug) Pre=1 Sub=2,
     * USART2 抢占级更高 -> LoRa 收字节能打断 debug 打印 ISR, 通信更稳 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* 恢复总中断使能 */
    __enable_irq();

    /* ⭐⭐⭐ 第一时间启动看门狗: 任何初始化步骤卡死(BSP_Init里任何一步)
     * 都会在 4s 后自动复位, 再也不会出现"必须手动按复位才好" */
    WDG_Init();

    /* 初始化所有板级外设(含LoRa模块) */
    BSP_Init();

    /* 上电横幅: 区分当前烧录的是节点端/网关端固件 */
    Usart_Printf(USART_DEBUG,
        "\r\n[SYS] ========== 智能停车场节点 %s ==========\r\n", NODE_FW_VERSION);

    /* ⭐ 本次复位原因 (供电跌落/看门狗卡死/按键/OTA) */
    PrintResetReason();

    /* 主循环 - 时间戳非阻塞架构 */
    static uint32_t s_lastWdgLogTick = 0;  /* 喂狗日志最后打印时间 */
    while (1)
    {
        US_Task();                  /* 超声波采样 */
        ParkingStatus_Check();      /* 车位状态检测 */
        QMC_Task();                 /* 地磁采集 */
        LED_Task();                 /* LED控制 */
        LoRa_Task();                /* LoRa通信: 响应网关轮询 + 下行命令 */

        /* ⭐ 喂狗状态日志 (必须放在喂狗之前: 打印卡死 = 不喂狗 = 4s复位, 符合预期) */
        if (Get_Tick() - s_lastWdgLogTick >= WDG_LOG_INTERVAL_MS)
        {
            s_lastWdgLogTick = Get_Tick();
            Usart_Printf(USART_DEBUG, "[WDG] 喂狗正常, 运行 %lu 秒\r\n",
                         (unsigned long)(Get_Tick() / 1000));
            /* ⭐ USART2 ISR 诊断: oreCount 持续涨=串口过载; rxCount 不涨=RX中断没触发,
             * 用于定位"LoRa 模块有输出但 STM32 收不到命令"类故障 */
            Usart_Printf(USART_DEBUG, "[USART2] ORE(溢出)=%u 收字节=%lu\r\n",
                         (unsigned)usart2_oreCount, (unsigned long)usart2_rxCount);
        }
		
        /* 最后一步才喂狗: 完整跑完全部任务才有资格, 任何一步卡死都不喂 */
        WDG_Feed();
    }
}
