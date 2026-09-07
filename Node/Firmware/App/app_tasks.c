/****************************************************************************
 * 应用层周期任务实现 - app_tasks.c
 * 
 * 功能描述:
 *   主循环中周期调用的任务函数实现(超声波/地磁/车位状态/OLED/WiFi)
 *   数据上报与下行指令处理
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "bsp_ultrasonic.h"
#include "bsp_qmc5883p.h"
#include "bsp_lora.h"
#include "node_config.h"   /* 节点身份运行时变量 g_nodeProductKey/g_nodeDeviceName */
#include "app_global.h"
#include "stm32f10x_flash.h"

/* ==================== 宏定义 ==================== */

/* -------- 采样/任务调度周期 -------- */
#define US_UPDATE_INTERVAL      100     /* 超声波采样周期(ms) */
#define QMC_UPDATE_INTERVAL     100     /* 地磁采样周期(ms) */
#define PARK_CHECK_INTERVAL     200     /* 车位状态检测周期(ms) */

/* -------- 传感器参数 -------- */
/* 超声波 */
#define US_SHIFT                2       /* EMA滤波系数 alpha=1/4 */
#define US_MIN_VALID            2       /* 超声波最小有效距离(cm) */
#define US_MAX_VALID            400     /* 超声波最大有效距离(cm) */
/* 地磁 */
#define MAG_DEBOUNCE_CNT        2       /* 地磁消抖连续计数次数 */
#define MAG_Z_SQ_THRESH         2.0f    /* 地磁Z轴磁场阈值 */

/* -------- 车位状态判定阈值 -------- */
#define DIST_THRESHOLD_CM       10      /* 超声波判断有车的距离阈值(cm) */

/* ⭐ 车离去抖: 连续 N 次检测(每次 PARK_CHECK_INTERVAL=200ms)无车,
 * 才判定车真正离开. 防止单次传感器毛刺(超声波假回波/地磁波动)瞬断
 * "车在"信号, 导致僵尸车占用计时被清零重计 */
#define CAR_ABSENT_DEBOUNCE     3       /* 3 × 200ms = 600ms */

/* -------- OTA 升级 -------- */
#include "app_version.h"    /* 版本单一源头: NODE_FW_VERSION */
#define OTA_FLAG_ADDR           0x0800FC00  /* 升级标志页地址 */
#define OTA_FLAG_GO             0xA5A5A5A5  /* 需要升级 */
#define OTA_FLAG_DONE           0x00000000  /* 升级完成 */

/* ==================== 全局变量定义 ==================== */

uint16_t Distance = 0;
ParkStatus_t ParkStatus = PARK_IDLE;
uint32_t OccupiedTime = 0;
uint32_t LastStatusChangeTick = 0;
uint8_t LEDAlarmEnable = 1;  /* 报警灯使能(僵尸车报警灯), 默认开启 */
QMC5883P_Device_t qmc5883p;
uint8_t MagCarPresent = 0;
NodeData_t NodeDataCache;
volatile uint8_t StatusChanged = 1;     /* 车位状态变化标志
                                         * 置位1: 有人/无人状态切换时
                                         * 触发立即上传, 不用等定时
                                         * 周期, 保证状态变化实时可见 */
uint32_t g_zombieThreshold = 3600;      /* ⭐ 僵尸车判定阈值(秒), 默认1小时 */
uint16_t g_sensorDistanceCm = DIST_THRESHOLD_CM;  /* ⭐ 超声波判定距离阈值(cm), 默认10cm, 可动态下发 */

/****************************************************************************
 * 函数名: US_Task
 * 功能:   超声波距离采样任务
 * 参数:   无
 * 返回:   无
 * 说明:   有效距离过滤 + 滑动滤波 (EMA)
 *         alpha = 1/4, 平滑距离采样波动, 避免抖动
 *         超过/低于/读取失败均视为无效, 保留上次有效
 *         滤波结果到 Distance
 ****************************************************************************/
void US_Task(void)
{
    static uint32_t lastUpdateTick = 0;
    static uint16_t smoothDist = 0;
    static uint8_t firstRun = 1;

    if (Get_Tick() - lastUpdateTick >= US_UPDATE_INTERVAL)
    {
        uint16_t raw = US_GetDistance();

        /* 有效距离过滤: 无效(=0), 超限, 失败 -> 保留上次有效值 */
        if (raw < US_MIN_VALID || raw > US_MAX_VALID)
            raw = smoothDist;

        /* 首次运行直接采用初始值 */
        if (firstRun)
        {
            smoothDist = raw;
            firstRun = 0;
        }
        else
        {
            /* EMA: output = (output * (2^N - 1) + input) >> N
             *      = output * 0.75 + input * 0.25   (N=2) */
            smoothDist = (uint16_t)(((uint32_t)smoothDist * ((1 << US_SHIFT) - 1) + raw) >> US_SHIFT);
        }

        Distance = smoothDist;

        /* ⭐ 诊断: 有车状态下距离出现异常尖峰(单次毛刺即可推越 10cm 阈值
         * 导致"车在"瞬断), 用于定位僵尸计时清零的根因是超声波还是地磁 */
        if (ParkStatus != PARK_IDLE && raw > (uint16_t)Distance + 10)
            Usart_Printf(USART_DEBUG, "[US][毛刺] raw=%dcm smooth=%dcm\r\n",
                         raw, Distance);

        lastUpdateTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: QMC_Task
 * 功能:   QMC5883P 地磁传感器任务
 * 参数:   无
 * 返回:   无
 * 说明:   每100ms读取一次Z轴磁场强度
 *         判定阈值: Z轴强度 > 阈值 判定有车
 *         连续2次才更新状态, 避免瞬间磁场波动干扰
 ****************************************************************************/
void QMC_Task(void)
{
    static uint32_t lastUpdateTick = 0;
    static uint8_t debounceCnt = 0;

    if (Get_Tick() - lastUpdateTick >= QMC_UPDATE_INTERVAL)
    {
        if (QMC5883P_Update(&qmc5883p) == QMC5883P_OK)
        {
            float z = QMC5883P_GetZ(&qmc5883p);
            float zSq = z * z;

            /* 判定阈值: Z轴磁场平方超过阈值 */
            uint8_t triggered = (zSq > MAG_Z_SQ_THRESH);

            /* --- 消抖处理: 连续计数 --- */
            if (triggered)
            {
                if (debounceCnt < MAG_DEBOUNCE_CNT)
                    debounceCnt++;
                if (debounceCnt >= MAG_DEBOUNCE_CNT)
                    MagCarPresent = 1;
            }
            else
            {
                if (debounceCnt > 0)
                    debounceCnt--;
                if (debounceCnt == 0)
                    MagCarPresent = 0;
            }

            /* ⭐ 诊断: 有车状态下地磁Z跌破阈值(当前仍判有车),
             * 可能触发"车在"瞬断, 用于定位根因是超声波还是地磁 */
            if (ParkStatus != PARK_IDLE && !triggered && MagCarPresent)
                Usart_Printf(USART_DEBUG, "[QMC][异常] z=%.2f zSq=%.2f (阈值%.1f) cnt=%d\r\n",
                             z, zSq, MAG_Z_SQ_THRESH, debounceCnt);
        }
        else
        {
            Usart_Printf(USART_DEBUG, "[QMC][ERR] 地磁更新失败\r\n");
        }

        lastUpdateTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: ParkingStatus_Check
 * 功能:   车位状态检测
 * 参数:   无
 * 返回:   无
 * 说明:   超声波与地磁联合判断车位占用状态
 *         距离 < 10cm 且 地磁 触发 判定有车
 *         连续占用超过设定时间判定为僵尸车位
 ****************************************************************************/
void ParkingStatus_Check(void)
{
    static uint32_t lastCheckTick = 0;
    static uint8_t absentCnt = 0;       /* ⭐ 车离连续计数(去抖) */

    if (Get_Tick() - lastCheckTick >= PARK_CHECK_INTERVAL)
    {
        uint8_t carPresent = (Distance > 0 && Distance < g_sensorDistanceCm) && MagCarPresent;

        /* ⭐ 车离去抖: 连续 CAR_ABSENT_DEBOUNCE 次检测无车才判定车离开.
         * 单次毛刺只累加计数不触发切换, 计时继续, 不再被清零 */
        if (carPresent)
            absentCnt = 0;
        else if (absentCnt < CAR_ABSENT_DEBOUNCE)
            absentCnt++;
        uint8_t carAbsent = (absentCnt >= CAR_ABSENT_DEBOUNCE);

        /* ⭐ 诊断: 有车状态下"车在"瞬断(去抖正保住计时不重置),
         * 打印是哪路信号掉下去, 用于确认根因 */
        if (ParkStatus != PARK_IDLE && !carPresent)
            Usart_Printf(USART_DEBUG, "[DBG] 车在瞬断: dist=%dcm mag=%d absent=%d/%d (去抖中)\r\n",
                         Distance, MagCarPresent, absentCnt, CAR_ABSENT_DEBOUNCE);

        switch (ParkStatus)
        {
            case PARK_IDLE:
                if (carPresent)
                {
                    ParkStatus = PARK_OCCUPIED;         /* 状态: 有车 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                    StatusChanged = 1;                  /* 标记状态变化 */
                }
                break;

            case PARK_OCCUPIED:
                if (carAbsent)
                {
                    ParkStatus = PARK_IDLE;             /* 状态: 无车 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                    StatusChanged = 1;                  /* 标记状态变化 */
                }
                else
                {
                    OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
                    if (OccupiedTime >= g_zombieThreshold)  /* ⭐ 动态阈值判定僵尸车 (秒级) */
                    {
                        ParkStatus = PARK_ZOMBIE;
                        StatusChanged = 1;              /* 标记状态变化 */
                    }
                }
                break;

            case PARK_ZOMBIE:
                OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
                if (carAbsent)
                {
                    ParkStatus = PARK_IDLE;             /* 状态: 无车 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                    StatusChanged = 1;                  /* 标记状态变化 */
                }
                else if (OccupiedTime < g_zombieThreshold)
                {
                    /* ⭐ 阈值被调大后, 当前占用时长不再达到阈值:
                     * 僵尸 → 退回"有车占用"(LED 熄灭), 计时连续不重置,
                     * 时长继续累计; 之后阈值再调小时会立即重新判定僵尸 */
                    ParkStatus = PARK_OCCUPIED;
                    StatusChanged = 1;                  /* 标记状态变化 */
                }
                break;
        }

        lastCheckTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: LED_Task
 * 功能:   LED指示灯控制任务
 * 参数:   无
 * 返回:   无
 * 说明:   使能时(LEDAlarmEnable=1), 根据车位状态控制LED亮灭
 *         僵尸车位->常亮, 正常->熄灭
 *         禁用时(LEDAlarmEnable=0), 强制熄灭LED
 ****************************************************************************/
void LED_Task(void)
{
    if (LEDAlarmEnable)
    {
        if (ParkStatus == PARK_ZOMBIE)
            LED_ON();
        else
            LED_OFF();
    }
    else
    {
        LED_OFF();
    }
}

/****************************************************************************
 * 函数名: PackNodeData
 * 功能:   打包节点传感器数据到结构体(替代原 GenerateParkingData)
 * 参数:   无
 * 返回:   无
 * 说明:   将所有传感器数据打包到 NodeDataCache 结构体,
 *         通过 LoRa 发送给网关, 网关代为上报 OneNET
 ****************************************************************************/
static void PackNodeData(void)
{
    NodeDataCache.ParkStatus    = (uint8_t)ParkStatus;
    NodeDataCache.GeoMagnetic   = MagCarPresent;
    NodeDataCache.Ultrasonic    = Distance;
    NodeDataCache.OccupiedTime  = OccupiedTime;
    NodeDataCache.LED           = LED_GetState() ? 1 : 0;   /* LED(报警灯)实际状态, 上报平台 LED 属性 */
    NodeDataCache.ZombieThreshold = g_zombieThreshold;   /* ⭐ 当前生效阈值上报给平台观看 */
    NodeDataCache.SensorDistanceCm = g_sensorDistanceCm;  /* ⭐ 当前生效超声波距离阈值上报 */
}

/****************************************************************************
 * 函数名: ota_version_compare
 * 功能:   字符串版本比较 (全程字符串方案)
 * 参数:   a/b - 版本字符串 (如 "v2.321" / "2.3.1"), 跳过前导 v/V
 * 返回:   >0 a>b, <0 a<b, =0 相等
 * 说明:   按 '.' 分段, 每段逐位解析为数字后比较,
 *         支持任意位数与段数, 空串/非法串按 0 段处理
 ****************************************************************************/
static int ota_version_compare(const char *a, const char *b)
{
    if (a == NULL) a = "";
    if (b == NULL) b = "";
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;

    while (*a || *b)
    {
        unsigned va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (unsigned)(*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (unsigned)(*b - '0'); b++; }
        if (va != vb)
            return (va > vb) ? 1 : -1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/****************************************************************************
 * 函数名: LoRa_CmdCallback
 * 功能:   LoRa 下行命令回调函数
 * 参数:   cmd - 命令名称 (如 "AT+DATA", "AT+CER", "AT+SetLed")
 *         value - 命令参数值 (如 "0", "1", 无参数时为NULL)
 * 返回:   无
 * 说明:   网关通过 LoRa 定点传输发送 AT 命令轮询节点,
 *         节点在回调中响应数据/证书, 或执行下行控制命令
 ****************************************************************************/
static void LoRa_CmdCallback(const char *cmd, const char *value)
{
    /* 打印收到的网关命令(带参数), 与发送打印成对, 一问一答清晰 */
    if (value != NULL)
        Usart_Printf(USART_DEBUG, "[LoRa] 收到<- 网关 命令: %s=%s\r\n", cmd, value);
    else
        Usart_Printf(USART_DEBUG, "[LoRa] 收到<- 网关 命令: %s\r\n", cmd);

    /* 注意: 命令名不携带节点编号(定点传输已按地址区分目标),
     *       因此只用前缀匹配命令名即可, 无需校验数字后缀
     */

    /* 网关查询数据: AT+DATA -> 发送传感器数据 */
    if (strncmp(cmd, "AT+DATA", 7) == 0)
    {
        PackNodeData();
        LoRa_Node_SendData(&NodeDataCache);
        StatusChanged = 0;
    }
    /* 网关查询证书: AT+CER -> 发送节点证书(含 OneNET 子设备身份) */
    else if (strncmp(cmd, "AT+CER", 6) == 0)
    {
        NodeCert_t cert;
        memset(&cert, 0, sizeof(cert));
        cert.valid = 1;
        strncpy(cert.ProductKey, g_nodeProductKey, sizeof(cert.ProductKey) - 1);
        strncpy(cert.DeviceName, g_nodeDeviceName, sizeof(cert.DeviceName) - 1);
        strncpy(cert.FwVersion, NODE_FW_VERSION, sizeof(cert.FwVersion) - 1);
        LoRa_Node_SendCert(&cert);
    }
    /* 平台 SetLed 服务 → 网关转发: AT+SetLed=<v> (报警灯使能 0/1) */
    else if (strcmp(cmd, "AT+SetLed") == 0)
    {
        if (value != NULL)
        {
            LEDAlarmEnable = (uint8_t)atoi(value);
            StatusChanged = 1;
            Usart_Printf(USART_DEBUG, "[CTRL] 报警灯使能 -> %d\r\n", LEDAlarmEnable);
        }
        LoRa_Node_SendAck("AT+SetLed");
    }
    /* ⭐ 网关下发僵尸车判定阈值: AT+ZombieThreshold=<秒数>
     * 默认3600秒(1小时), APP端可动态下发覆盖, 支持演示用短阈值 */
    else if (strcmp(cmd, "AT+ZombieThreshold") == 0)
    {
        if (value != NULL)
        {
            long v = strtol(value, NULL, 10);
            if (v >= 5 && v <= 2592000)  /* 允许范围: 5秒 ~ 30天 */
            {
                g_zombieThreshold = (uint32_t)v;
                Usart_Printf(USART_DEBUG, "[CTRL] 僵尸车阈值 -> %lu秒\n", (unsigned long)g_zombieThreshold);
            }
        }
        LoRa_Node_SendAck("AT+ZombieThreshold");
    }
    /* ⭐ 网关下发超声波判定距离阈值: AT+SensorDistance=<cm>
     * 默认10cm, APP端可动态下发覆盖 */
    else if (strcmp(cmd, "AT+SensorDistance") == 0)
    {
        if (value != NULL)
        {
            long v = strtol(value, NULL, 10);
            if (v >= US_MIN_VALID && v <= US_MAX_VALID)  /* 允许范围: 2cm ~ 400cm (传感器有效量程) */
            {
                g_sensorDistanceCm = (uint16_t)v;
                Usart_Printf(USART_DEBUG, "[CTRL] 超声波距离阈值 -> %ucm\r\n", g_sensorDistanceCm);
            }
        }
        LoRa_Node_SendAck("AT+SensorDistance");
    }
    /* 网关心跳查询: AT+PING -> 回复 PONG */
    else if (strcmp(cmd, "AT+PING") == 0)
    {
        LoRa_Node_SendAck("PONG");
    }
    /* 网关触发 OTA 升级: AT+OTA=start,<版本串>
     * 触发后: 回复ACK → 写升级标志 → 复位 → BootLoader 接收固件
     * ⭐ 先回复ACK再复位: 防 LoRa 丢包导致网关盲等,
     * 网关收到ACK才进入复位等待, 收不到则重发命令 */
    else if (strcmp(cmd, "AT+OTA") == 0)
    {
        /* 提取目标版本串: 兼容 "start,V2.321" / "start,v2.321" / 纯版本串 */
        const char *targetVer = NULL;
        if (value != NULL)
        {
            const char *p = value;
            if (strncmp(p, "start,", 6) == 0)
                p += 6;
            if (*p == 'V' || *p == 'v')
                p++;
            targetVer = p;
        }

        /* 版本校验: 目标版本 > 当前版本 或 未指定版本时强制升级 */
        if (targetVer == NULL || *targetVer == '\0' ||
            ota_version_compare(targetVer, NODE_FW_VERSION) > 0)
        {
            /* ⭐ 先回复ACK: 让网关确认收到命令, 再写标志复位 */
            LoRa_Node_SendAck("AT+OTA:ack");
            Usart_Printf(USART_DEBUG, "[OTA] 已回复ACK, 写升级标志...\r\n");

            FLASH_Unlock();
            FLASH_ErasePage(OTA_FLAG_ADDR);
            FLASH_ProgramHalfWord(OTA_FLAG_ADDR, (uint16_t)(OTA_FLAG_GO & 0xFFFF));
            FLASH_ProgramHalfWord(OTA_FLAG_ADDR + 2, (uint16_t)((OTA_FLAG_GO >> 16) & 0xFFFF));
            FLASH_Lock();

            Usart_Printf(USART_DEBUG, "[OTA] 升级标志已写入, 即将复位...\r\n");
            /* 200ms: 确保 LoRa 模块完成 ACK 无线发送 + Flash 写入稳定 */
            DelayXms(200);
            NVIC_SystemReset();
        }
        else
        {
            LoRa_Node_SendAck("AT+OTA:version_ok");
            Usart_Printf(USART_DEBUG, "[OTA] 版本已最新(%s), 跳过升级\r\n", NODE_FW_VERSION);
        }
    }
    else
    {
        Usart_Printf(USART_DEBUG, "[LoRa][ERR] 未知命令: %s\r\n", cmd);
    }
}

/****************************************************************************
 * 函数名: LoRa_Task
 * 功能:   LoRa 通信任务(替代原 Wifi_Task)
 * 参数:   无
 * 返回:   无
 * 说明:   轮询接收网关命令并响应, 采用网关轮询模式:
 *         - 网关定时发送 AT+DATA 查询数据 → 节点回传 NodeData
 *         - 网关首次发送 AT+CER 查询证书 → 节点回传证书
 *         - 网关转发平台 SetLed 服务命令 AT+SetLed=0/1 → 节点执行并确认
 *         节点不主动发送, 避免多节点 LoRa 碰撞
 ****************************************************************************/
void LoRa_Task(void)
{
    LoRa_Node_Poll(LoRa_CmdCallback);
}
