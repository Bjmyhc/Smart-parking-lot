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

#include <stdio.h>
#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "bsp_ultrasonic.h"
#include "bsp_oled.h"
#include "bsp_qmc5883p.h"
#include "esp8266.h"
#include "onenet.h"
#include "mqttkit.h"
#include "device_config.h"
#include "app_global.h"

/* ==================== 宏定义 ==================== */

/* -------- 采样/任务调度周期 -------- */
#define US_UPDATE_INTERVAL      100     /* 超声波采样周期(ms) */
#define QMC_UPDATE_INTERVAL     100     /* 地磁采样周期(ms) */
#define PARK_CHECK_INTERVAL     200     /* 车位状态检测周期(ms) */
#define OLED_UPDATE_INTERVAL    250     /* OLED刷新周期(ms) */
#define UPLOAD_INTERVAL         15000   /* WiFi上报周期(ms)
                                         * 上传周期(15s): LoRa低带宽通道,
                                         * 发送(TX)耗时较长, 会占用链路,
                                         * 若频繁上报会造成拥堵, 3s间隔容易
                                         * 产生大量待发送数据帧!
                                         * 仅当StatusChanged被置位时
                                         * 可立即上传(状态变化触发) + 定时
                                         * occTimer每1s递增统计占用时长 */

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

/* -------- WiFi / OneNET / MQTT 通信 -------- */
/* 断线重连 */
#define WIFI_RECONNECT_DELAY    2000    /* 重连间隔(ms) */
#define WIFI_MAX_RETRIES        10       /* 重连最大次数 */
#define WIFI_RECONNECT_INTERVAL 30000   /* WiFi重连尝试最大间隔时间(ms) */

/* 心跳与链路活性检测 */
#define HEARTBEAT_INTERVAL_MS   20000   /* MQTT发送PINGREQ心跳包间隔(ms)
                                         * 必须小于CONNECT keepalive(20s)时间,
                                         * 用于检测TCP链路, 特别LoRa链路
                                         * 因为信道较窄较慢(约256s超时) */
#define LINK_DEAD_TIMEOUT_MS    25000   /* 链路失效判定时间(ms)
                                         * 小于心跳间隔(20s): 如果收到过
                                         * 任何消息(包括PINGRESP),
                                         * 判定链路正常, 不主动断开.
                                         * PINGRESP发送间隔(心跳帧0x00),
                                         * 故20s内必然更新lastRxTick, 25s
                                         * 无数据则判定PINGRESP丢失, 链路断开 */

/* 上线后补发(防QoS0丢包) */
#define CONNECT_RESEND_TIMES    3       /* 上线后重发次数
                                         * MQTT QoS0消息可能丢失: "Data
                                         * uploaded!"发送时可能尚未连上ESP8266
                                         * 透传, 造成消息丢失. 上线/重连成功后
                                         * 重发几次可确保LoRa信道中的数据帧被
                                         * 正确送达, 也能覆盖因OneNET的
                                         * 下行query指令造成的冲突帧丢失.
                                         * 每次3帧等间隔重发, 代价不大,
                                         * 确保上位机看到最新设备状态 */
#define CONNECT_RESEND_INTERVAL 1500    /* 重发间隔(ms)
                                         * 若SUBSCRIBE尚未订阅完成就重发, 需
                                         * TX发送占用的时间, 因此预留出TX时间
                                         * LoRa信道占用 */

/* ==================== 全局变量定义 ==================== */

char PublishBuf[256];
const char PubTopic[] = TOPIC_PROPERTY_POST;
const char *SubTopic[] = {TOPIC_PROPERTY_SET};
unsigned char *pData = NULL;

uint16_t Distance = 0;
ParkStatus_t ParkStatus = PARK_IDLE;
uint32_t OccupiedTime = 0;
uint32_t LastStatusChangeTick = 0;
uint8_t LEDEnable = 1;
QMC5883P_Device_t qmc5883p;
uint8_t MagCarPresent = 0;
uint8_t WifiConnected = 0;
volatile uint8_t StatusChanged = 1;     /* 车位状态变化标志
                                         * 置位1: 有人/无人状态切换时
                                         * 触发立即上传, 不用等定时
                                         * 周期, 保证状态变化实时可见 */

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

            /* 调试打印 */
            //Usart_Printf(USART_DEBUG, "QMC: Z=%.2f, zSq=%.1f, cnt=%d, car=%d\r\n",z, zSq, debounceCnt, MagCarPresent);
        }
        else
        {
            Usart_Printf(USART_DEBUG, "QMC: Update FAIL!\r\n");
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

    if (Get_Tick() - lastCheckTick >= PARK_CHECK_INTERVAL)
    {
        uint8_t carPresent = (Distance > 0 && Distance < DIST_THRESHOLD_CM) && MagCarPresent;

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
                if (!carPresent)
                {
                    ParkStatus = PARK_IDLE;             /* 状态: 无车 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
                    StatusChanged = 1;                  /* 标记状态变化 */
                }
                else
                {
                    OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
                    if (OccupiedTime > 5)               /* 僵尸判定: 5秒后进入 */
                    {
                        ParkStatus = PARK_ZOMBIE;
                        StatusChanged = 1;              /* 标记状态变化 */
                    }
                }
                break;

            case PARK_ZOMBIE:
                OccupiedTime = (Get_Tick() - LastStatusChangeTick) / 1000;
                if (!carPresent)
                {
                    ParkStatus = PARK_IDLE;             /* 状态: 无车 */
                    LastStatusChangeTick = Get_Tick();
                    OccupiedTime = 0;
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
 * 说明:   使能时(LEDEnable=1), 根据车位状态控制LED亮灭
 *         僵尸车位->常亮, 正常->熄灭
 *         禁用时(LEDEnable=0), 强制熄灭LED
 ****************************************************************************/
void LED_Task(void)
{
    if (LEDEnable)
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
 * 函数名: OLED_Task
 * 功能:   OLED屏幕显示任务
 * 参数:   无
 * 返回:   无
 * 说明:   定时刷新, 每次更新OLED显示内容
 *         显示WiFi连接状态与车位状态信息
 ****************************************************************************/
void OLED_Task(void)
{
    static uint32_t lastUpdateTick = 0;

    if (Get_Tick() - lastUpdateTick >= OLED_UPDATE_INTERVAL)
    {
        if (WifiConnected)
            OLED_ShowCH(0, 0, (u8 *)"WiFi已连接");
        else
            OLED_ShowCH(0, 0, (u8 *)"WiFi未连接");

        OLED_Printf(0, 2, "地磁: %d", MagCarPresent);
        OLED_Printf(0, 4, "距离: %.3d cm", Distance);
        OLED_ShowCH(0, 6, (u8 *)"状态: ");
        switch (ParkStatus)
        {
            case PARK_IDLE:
                OLED_Printf(48, 6, "空闲     ");
                break;
            case PARK_OCCUPIED:
                OLED_Printf(48, 6, "有车 %3ds", OccupiedTime);
                break;
            case PARK_ZOMBIE:
                OLED_Printf(48, 6, "僵尸车   ");
                break;
            default:
                OLED_Printf(48, 6, "未知     ");
                break;
        }

        lastUpdateTick = Get_Tick();
    }
}

/****************************************************************************
 * 函数名: GenerateParkingData
 * 功能:   生成上报数据JSON格式
 * 参数:   无
 * 返回:   无
 *
 * 数据说明:
 *   ParkStatus:    车位状态 (0=空闲, 1=有车, 2=僵尸占用)
 *   GeoMagnetic:   地磁检测值
 *   Ultrasonic:    超声波距离值(cm)
 *   OccupiedTime:  占用时长(秒)
 *   LED:           LED状态值 (true=亮起, false=熄灭, 只读展示)
 *   LedEnable:     使能状态值 (true=开启, false=关闭, 可远程控制)
 ****************************************************************************/
void GenerateParkingData(void)
{
    sprintf(PublishBuf,
            "{\"id\":\"%u\",\"params\":{"
            "\"ParkStatus\":{\"value\":%d},"
            "\"GeoMagnetic\":{\"value\":%d},"
            "\"Ultrasonic\":{\"value\":%d},"
            "\"OccupiedTime\":{\"value\":%d},"
             "\"LED\":{\"value\":%s},"
             "\"LedEnable\":{\"value\":%s}}}",
            (unsigned int)Get_Tick(),
            ParkStatus,
            MagCarPresent,
            Distance,
            OccupiedTime,
            LED_GetState() ? "true" : "false",
            LEDEnable ? "true" : "false");
}

/****************************************************************************
 * 函数名: Wifi_Reconnect
 * 功能:   WiFi断线重连处理函数
 * 参数:   无
 * 返回:   无
 * 说明:   检测到 WifiConnected=0 时调用
 *         每次重试间隔逐渐增大, 避免频繁重试(影响正常通信)
 *         超过30秒重试间隔上限, 会重置TCP(CIPCLOSE+CIPSTART)
 *         重新建立MQTT CONNECT并保持Lora通信正常
 ****************************************************************************/
static void Wifi_Reconnect(void)
{
    static uint32_t lastReconnectTick = 0;
    static uint8_t firstCall = 1;
    uint8_t i;

    /* 首次调用时初始化时间基准, 间隔30秒后
     * 才会尝试Wifi_Init重新初始化ESP8266的TCP连接 */
    if (firstCall)
    {
        lastReconnectTick = Get_Tick();
        firstCall = 0;
        Usart_Printf(USART_DEBUG, "WiFi offline, wait before reconnect...\n");
        return;
    }

    /* 超过30秒重试间隔才执行 */
    if (Get_Tick() - lastReconnectTick < WIFI_RECONNECT_INTERVAL)
        return;
    lastReconnectTick = Get_Tick();

    Usart_Printf(USART_DEBUG, "WiFi offline, reconnecting...\n");

    for (i = 0; i < WIFI_MAX_RETRIES; i++)
    {
        OLED_ShowCH(0, 0, (u8 *)"WiFi重连中...");
        OLED_Printf(0, 2, "(%d/3)...", i + 1);
        Usart_Printf(USART_DEBUG, "Reconnect (%d/3)...\n", i + 1);

        /* 先退出透传模式, 发送AT指令(CIPCLOSE/CIPSTART等)
         * 确保ESP8266处于命令模式, 方便控制 */
        ESP8266_ExitTransparent();

        /* 本次重试需要重新初始化ESP8266(包括CIPCLOSE+CIPSTART+重新连接)
         * 然后MQTT CONNECT建立TCP连接, 保持
         * Init内部(ESP8266连接/断开连接)成功后调用DevLink, 并订阅数据 */
        if (ESP8266_Init() != 0)
        {
            Usart_Printf(USART_DEBUG, "ESP8266 Init failed, skip DevLink\n");
            continue;
        }

        /* 等待Lora数据缓冲/TCP缓冲清空后再连接MQTT */
        DelayXms(2000);

        if (OneNet_DevLink() == 0)
        {
            OLED_ShowCH(0, 0, (u8 *)"WiFi连接成功!");
            OneNet_Subscribe(SubTopic, 1);
            WifiConnected = 1;
            DelayXms(500);
            OLED_Clear();
            Usart_Printf(USART_DEBUG, "Reconnect OK!\n");
            return;
        }

        DelayXms(WIFI_RECONNECT_DELAY);
    }

    OLED_ShowCH(0, 0, (u8 *)"WiFi连接失败!");
    Usart_Printf(USART_DEBUG, "Reconnect FAILED!\n");
    DelayXms(1000);
    OLED_Clear();
}

/****************************************************************************
 * 函数名: Wifi_Task
 * 功能:   WiFi通信与数据上报任务
 * 参数:   无
 * 返回:   无
 * 说明:   定时(15s)上报传感器数据到OneNET平台
 *         断线时进行自动重连(最多3次)
 *
 *         结合Lora链路本身的特性(低速信道):
 *         1. 定时上传(15s): LoRa使节点设备占据信道(TX)时
 *            耗时较长, 应避免频繁上传造成链路拥塞. 状态变化(3s)
 *            内可立即上传, 覆盖实时性需求.
 *         2. 节点设备上传时LoRa信道处于RX状态, 下行指令到达
 *            会先进入节点设备的缓冲区, 无法立即上传
 *            ESP8266_GetIPD解析下行指令, 可能造成部分丢失.
 *         3. 每条上行数据到达平台后都会触发下行, 包括ESP8266_Clear
 *            等操作(对应平台下行ack/命令响应).
 *         4. 上行发送期间请勿执行下行操作, 避免Delay阻塞等
 *            OLED/显示刷新操作.
 ****************************************************************************/
void Wifi_Task(void)
{
    static uint32_t lastUploadTick = 0;
    static uint32_t lastHeartbeatTick = 0;
    static uint8_t  wasConnected = 0;       /* 记录上次连接状态(用于上升沿检测) */
    static uint8_t  resendRemain = 0;       /* 剩余待重发帧数 */
    static uint32_t lastResendTick = 0;     /* 上次重发时间戳(ms) */

    /* === 上线后立即重发 ===
     * 刚"连上"(上升沿: 断开->连接成功), 立即重发:
     * MQTT QoS0消息可能丢失, 上线/重连成功后仍可能丢在LoRa
     * 上行链路/平台缓冲中, 主动重发几次, 确保
     * query指令后设备状态更新. 每次3帧(间隔1.5s)依次重发
     * 减少碰撞, 保证上位机看到最新设备状态 */
    if (WifiConnected && !wasConnected)
    {
        resendRemain = CONNECT_RESEND_TIMES;
        lastResendTick = Get_Tick();
        Usart_Printf(USART_DEBUG, "Online! resend x%d to refresh snapshot...\n",
                     CONNECT_RESEND_TIMES);
    }
    wasConnected = WifiConnected;

    /* 未连接时执行重连逻辑, 并跳过本轮其他操作 */
    if (!WifiConnected)
    {
        Wifi_Reconnect();
        return;
    }

    /* === 链路活性检测(心跳超时) ===
     * 超过阈值未收到任何下行消息(包括PINGRESP) 视为 链路断开,
     * 主动断开重连, 避免长时间假在线(收不到任何响应,
     * TCP连接已断但设备未感知, 不主动断开, 一直假在线) */
    if (Get_Tick() - ESP8266_GetLastRxTime() > LINK_DEAD_TIMEOUT_MS)
    {
        Usart_Printf(USART_DEBUG, "Link dead (no RX %dms), force reconnect...\r\n",
                     (unsigned int)(Get_Tick() - ESP8266_GetLastRxTime()));
        WifiConnected = 0;
        return;
    }

    /* === MQTT心跳(PINGREQ) ===
     * 每60s发送一次, 并等待PINGRESP(2字节响应包), 维持TCP连接,
     * 并更新链路活性: PINGRESP到达后会刷新lastRxTick */
    if (Get_Tick() - lastHeartbeatTick >= HEARTBEAT_INTERVAL_MS)
    {
        MQTT_PACKET_STRUCTURE pingPacket = {NULL, 0, 0, 0};
        if (MQTT_PacketPing(&pingPacket) == 0)
        {
            ESP8266_SendData(pingPacket._data, pingPacket._len);
            MQTT_DeleteBuffer(&pingPacket);
            lastHeartbeatTick = Get_Tick();
            Usart_Printf(USART_DEBUG, "Heartbeat PINGREQ sent\r\n");
        }
    }

    /* 上报条件判断: 定时到 或 状态变化(如车位占用变化) 或 重发中
     * 说明: 状态变化(车位/地磁变化)>0 或 剩余重发帧数>=阈值,
     * 则 1.5s 间隔连续重发最多3次, 应对QoS0消息可能丢失的情况 */
    if ((Get_Tick() - lastUploadTick >= UPLOAD_INTERVAL) || StatusChanged ||
        (resendRemain > 0 && (Get_Tick() - lastResendTick >= CONNECT_RESEND_INTERVAL)))
    {
        /* 清除状态变化标记, 避免重复上报 */
        StatusChanged = 0;

        /* 上报前先处理完下行指令缓存, 确保没有残留的LoRa帧.
         * 处理逻辑: OneNet_RevPro 每帧解析并做业务处理, 可处理多帧
         * 直到缓存为空. 处理过程中可能触发状态位变化(如LED/α设置)
         * 部分会等待下次上报周期再处理 */
        while ((pData = ESP8266_GetIPD(0)) != NULL)
        {
            OneNet_RevPro(pData);
        }

        GenerateParkingData();
        OneNet_Publish(PubTopic, PublishBuf);
        lastUploadTick = Get_Tick();
        if (resendRemain > 0)
        {
            /* 重发流程: 递减计数, 更新时间戳以保持间隔 */
            resendRemain--;
            lastResendTick = Get_Tick();
            Usart_Printf(USART_DEBUG, "Data uploaded! (%d resend left)\n", resendRemain);
        }
        else
        {
            Usart_Printf(USART_DEBUG, "Data uploaded!\n");
        }

        /* 本次已上传, 避免本轮剩余逻辑重复执行(直接返回) */
        return;
    }

    /* === 下行指令处理(无上报时, 轮询处理) ===
     * LoRa链路: 节点设备长时间占用信道RX, 下行指令到达
     * ESP8266缓冲区, 必须及时读取, 否则可能丢失. 这里在
     * 非上报周期内轮询: RevPro 逐条解析并立即响应, 且同时处理
     * 多条(无残留处理则清空缓存) */
    while ((pData = ESP8266_GetIPD(0)) != NULL)
    {
        OneNet_RevPro(pData);
    }
}
