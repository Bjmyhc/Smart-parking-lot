﻿/****************************************************************************
 * 车牌子系统应用层 - app_plate.c
 *
 * 功能描述:
 *   节点侧车牌识别接入实现。参照《车牌接入-落地手册》§2.1 / §2.2:
 *   - 单命令通道三态状态机(PC_IDLE / PC_RUN_SENT / PC_PROBE_SENT),
 *     拍照与探活共用同一条通道, 半双工口同时只允许一条在途命令;
 *   - $PLATE 结果解析 + 归属判定(只认本次窗口首行, 发送前清 RX 缓冲);
 *   - PlateCache 车牌缓存;
 *   - 自动触发判定(车位状态变化 + 策略命中 + 冷却 PLATE_COOLDOWN_MS);
 *   - 周期探活维护 g_camOnline;
 *   - 全程非阻塞: 每轮只做"取缓冲 → 喂状态机 → 查超时", 不死等毫秒,
 *     否则会踩死网关 PING 的 500ms 超时。
 *
 * 作者: Bjmyhc
 * 日期: 2026-10-09 (v4 车牌子系统接入)
 ****************************************************************************/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "stm32f10x.h"
#include "bsp_delay.h"
#include "bsp_usart.h"
#include "bsp_lora.h"
#include "node_config.h"
#include "app_global.h"
#include "app_plate.h"

/* ==================== 本地常量 ==================== */

/* 摄像头命令(均以 \r\n 结尾, 与 P5.80 节点口约定一致) */
#define PLATE_CMD_RUN    "AT+RUN\r\n"
#define PLATE_CMD_INFO   "AT+INFO\r\n"
#define PLATE_CMD_THUMB  "AT+THUMB\r\n"   /* S5: 取缩略图, 摄像头回 $IMGD/$IMGB/$IMGE 行 */

/* 单行接收上限: 摄像头单行上限 95B, 超 127B 必是噪声 -> 整行丢弃
 * (S5 缩略图 $IMGB 数据行 = 6 + 60*2 = 126 字符, 恰在上限内) */
#define PLATE_LINE_MAX   128

/* 进入 PC_IDLE 后需停留的时间才允许发探活(避免刚收尾就发, 见手册 §2.1) */
#define PLATE_IDLE_SETTLE_MS   2000UL

/* S5: $IMGD..$IMGE 全程超时(7 行 @115200 约 70ms, 放宽到 1.5s 兜底丢行) */
#define PLATE_THUMB_TIMEOUT_MS 1500UL

/* ==================== 全局变量定义 ==================== */
PlateCache_t     g_plateCache;
volatile uint8_t g_plateFetchPending = 0;
volatile uint8_t g_camOnline         = 0;
uint8_t          g_capturePolicy     = CAPTURE_POLICY_DEFAULT;

/* S5 车牌缩略图(见 app_plate.h 注释) */
uint8_t          g_thumbCache[LORA_IMG_BYTES];
volatile uint8_t g_thumbValid  = 0;
volatile uint8_t g_imgFetchPending = 0;
volatile uint16_t g_thumbNo    = 0;

/* ==================== 状态机内部变量 ==================== */
typedef enum {
    PC_IDLE = 0,        /* 无在途命令 */
    PC_RUN_SENT,        /* 已发 AT+RUN, 等 $PLATE 结果 */
    PC_PROBE_SENT,      /* 已发 AT+INFO, 等任意一行 */
    PC_THUMB_SENT       /* S5: 已发 AT+THUMB, 等 $IMGD..$IMGE(行由全局分派处理) */
} PlateState_t;

static PlateState_t s_state = PC_IDLE;
static uint32_t s_sentAt;           /* 本状态命令发出时刻 */
static uint32_t s_idleSince;        /* 进入 PC_IDLE 的时刻 */
static uint32_t s_bootAt;           /* 初始化时刻(上电静默期基准) */
static uint32_t s_lastProbeAt;      /* 上次探活发出时刻 */

static uint8_t  s_captureReq;       /* 1=有待发拍照请求 */
static uint8_t  s_captureSrc;       /* 请求来源(PLATE_SRC_*) */
static uint8_t  s_retryCnt;         /* 本次拍照已重试次数 */
static uint8_t  s_awaitRetry;       /* 1=拍照超时后等重试间隔 */
static uint32_t s_retryAt;          /* 重试等待起点 */
static uint8_t  s_runGotResult;     /* 本次拍照窗口是否已收结果 */

static uint8_t  s_probeFail;        /* 连续探活失败次数 */

/* 自动触发 */
static ParkStatus_t s_prevStatus;   /* 上次车位状态(判上升沿) */
static uint8_t  s_autoArmed;        /* 本次状态变化是否已触发过 */
static uint32_t s_lastAutoAt;       /* 上次自动触发时刻(冷却用) */

/* 行接收缓冲 */
static char     s_line[PLATE_LINE_MAX];
static uint16_t s_lineLen;
static uint8_t  s_lineDiscard;      /* 1=当前行超长, 丢弃到行尾 */

/* S5 缩略图接收(摄像头 AT+THUMB 应答行) */
static uint8_t  s_thumbReq;         /* 1=识别结果刚有效, 待发 AT+THUMB */
static uint8_t  s_rxOn;             /* 1=$IMGD 已开收, 等数据行/结束行 */
static uint16_t s_rxLen;            /* 本图期望总长(282) */
static uint16_t s_rxPos;            /* 已收字节 */
static uint16_t s_rxNo;             /* 本图 imgNo(结束行校验通过才提交) */

/* 诊断计数(仅日志) */
static uint16_t s_camTimeout;       /* 拍照超时放弃次数 */
static uint16_t s_camPreempted;     /* 被抢占(ERR:)次数 */
static uint16_t s_strayPlate;       /* 非本次窗口的 $PLATE 行数 */

/* ==================== 内部函数声明 ==================== */
static void Plate_OnLine(const char *line);
static void Plate_ParseResult(const char *line);
static void Plate_OnThumbLine(const char *line);   /* S5: $IMGD/$IMGB/$IMGE */

/* ==================== 内部函数实现 ==================== */

/****************************************************************************
 * 发 AT+RUN, 进入 PC_RUN_SENT(发送前清 RX 缓冲, 丢弃上一条残字节)
 * 首次发送重置重试计数与结果旗子
 ****************************************************************************/
static void Plate_SendRun(void)
{
    Usart3_FlushRx();
    Usart3_SendAsync((const uint8_t *)PLATE_CMD_RUN, sizeof(PLATE_CMD_RUN) - 1);
    s_sentAt        = Get_Tick();
    s_retryCnt      = 0;
    s_awaitRetry    = 0;
    s_runGotResult  = 0;
    s_state         = PC_RUN_SENT;
}

/****************************************************************************
 * 发 AT+INFO, 进入 PC_PROBE_SENT
 ****************************************************************************/
static void Plate_SendProbe(void)
{
    Usart3_FlushRx();
    Usart3_SendAsync((const uint8_t *)PLATE_CMD_INFO, sizeof(PLATE_CMD_INFO) - 1);
    s_sentAt       = Get_Tick();
    s_lastProbeAt  = Get_Tick();
    s_state        = PC_PROBE_SENT;
}

/****************************************************************************
 * 策略是否允许对当前车位状态拍照
 * 0=不拍 1=有车拍 2=僵尸车拍 3=都拍
 ****************************************************************************/
static uint8_t Plate_PolicyAllows(ParkStatus_t st)
{
    switch (g_capturePolicy)
    {
        case 1:  return (st == PARK_OCCUPIED);
        case 2:  return (st == PARK_ZOMBIE);
        case 3:  return (st != PARK_IDLE);
        default: return 0;
    }
}

/****************************************************************************
 * PC_IDLE 时挑选下一条要发的命令: 拍照优先, 其次探活
 ****************************************************************************/
static void Plate_TryNextCommand(void)
{
    uint32_t now = Get_Tick();

    /* 上电静默期: 等摄像头模组启动就绪 */
    if ((uint32_t)(now - s_bootAt) < PLATE_ARM_DELAY_MS)
        return;

    /* ① 拍照优先 */
    if (s_captureReq)
    {
        Plate_SendRun();
        Usart_Printf(USART_DEBUG, "[PLATE] -> AT+RUN (src=%d)\r\n", s_captureSrc);
        return;
    }

    /* ② S5 取缩略图: 刚识别出有效车牌 -> 立刻向摄像头取图
     *    (不等探活静默: 图要趁新; 探活让路, 1.5s 内必有结局) */
    if (s_thumbReq)
    {
        Usart3_FlushRx();
        Usart3_SendAsync((const uint8_t *)PLATE_CMD_THUMB, sizeof(PLATE_CMD_THUMB) - 1);
        s_sentAt = Get_Tick();
        s_state  = PC_THUMB_SENT;
        Usart_Printf(USART_DEBUG, "[PLATE] -> AT+THUMB 取缩略图\r\n");
        return;
    }

    /* ③ 探活: 距上次探活 >= 周期 且 已在 IDLE 停留 >= 2s */
    if ((uint32_t)(now - s_lastProbeAt) >= PLATE_PROBE_MS &&
        (uint32_t)(now - s_idleSince)  >= PLATE_IDLE_SETTLE_MS)
    {
        Plate_SendProbe();
    }
}

/****************************************************************************
 * 自动触发判定: 车位状态变化(上升沿)且策略命中且冷却已过 -> 请求拍照
 ****************************************************************************/
static void Plate_CheckAutoTrigger(void)
{
    uint32_t now = Get_Tick();

    /* 上电静默期不触发 */
    if ((uint32_t)(now - s_bootAt) < PLATE_ARM_DELAY_MS)
        return;

    /* 车位状态变化: 重新武装(无稳定窗, 状态一变即可触发) */
    if (ParkStatus != s_prevStatus)
    {
        /* ⭐ 车离开(转 PARK_IDLE): 立即清空车牌缓存并经既有 0xF1 流请求网关取走.
         * 车牌是"最近一次识别结果"事件量, 若不在车走时清空, 下辆车进场、而本策略
         * (如僵尸车=2)尚未触发拍照的这段"空档期", App 仍会显示上一辆车的旧车牌.
         * 清为 '-' 并置"待取"旗子 → 网关下次 pack/post 上报一次 PlateNumber='-',
         * 平台与 App 随即不再显示旧牌(空档期显示"未识别", 待真识别结果覆盖). */
        if (ParkStatus == PARK_IDLE)
        {
            memset(&g_plateCache, 0, sizeof(g_plateCache));
            strcpy(g_plateCache.plate, "-");
            g_plateFetchPending = 1;
            /* S5: 车走了同时清缩略图 —— 不清则下辆车进场的空档期,
             * 网关/平台仍挂着上一辆车的图(与车牌 '-' 同步清, 语义一致) */
            g_thumbValid     = 0;
            g_imgFetchPending = 0;
            s_thumbReq       = 0;
            s_rxOn           = 0;
            Usart_Printf(USART_DEBUG, "[PLATE] 车已离开, 清空车牌缓存与缩略图\r\n");
        }
        s_prevStatus = ParkStatus;
        s_autoArmed  = 0;
    }

    if (s_autoArmed)                                   /* 本次变化已触发过 */
        return;
    if (!Plate_PolicyAllows(ParkStatus))
        return;
    if ((uint32_t)(now - s_lastAutoAt) < PLATE_COOLDOWN_MS)
        return;

    s_autoArmed  = 1;
    s_lastAutoAt = now;
    Plate_RequestCapture(PLATE_SRC_AUTO);
    Usart_Printf(USART_DEBUG, "[PLATE] 自动触发拍照 (状态=%d 策略=%d)\r\n",
                 (int)ParkStatus, g_capturePolicy);
}

/****************************************************************************
 * 解析单行文本, 按 '\n' 切行后入状态机
 ****************************************************************************/
static void Plate_FeedBytes(const uint8_t *buf, uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; i++)
    {
        char c = (char)buf[i];

        if (c == '\n')
        {
            if (!s_lineDiscard)
            {
                /* 去掉行尾空白(\r/空格) */
                while (s_lineLen > 0 &&
                       (s_line[s_lineLen - 1] == '\r' || s_line[s_lineLen - 1] == ' '))
                    s_lineLen--;

                if (s_lineLen > 0)
                {
                    s_line[s_lineLen] = '\0';
                    Plate_OnLine(s_line);
                }
            }
            s_lineLen     = 0;
            s_lineDiscard = 0;
        }
        else if (c == '\r')
        {
            /* 行内 \r 忽略(以 \n 为行界) */
        }
        else
        {
            if (s_lineDiscard)
                continue;

            if (s_lineLen < (uint16_t)(sizeof(s_line) - 1))
            {
                s_line[s_lineLen++] = c;
            }
            else
            {
                /* 超长: 丢弃整行 */
                s_lineDiscard = 1;
                s_lineLen     = 0;
            }
        }
    }
}

/****************************************************************************
 * 解析 $PLATE,<车牌>,<置信度>,<帧号>[,img=..] 结果行
 * 只读前 3 段; 容忍 '-'、空字段; 未识别置信度恒 0
 ****************************************************************************/
static void Plate_ParseResult(const char *line)
{
    const char *p = line + 7;    /* 跳过 "$PLATE," */
    const char *comma;
    char     segPlate[24];
    char     segConf[8];
    uint16_t len;

    /* 段1: 车牌 */
    comma = strchr(p, ',');
    if (comma == NULL)
        return;                  /* 结构不符, 不更新缓存 */
    len = (uint16_t)(comma - p);
    if (len >= sizeof(segPlate)) len = sizeof(segPlate) - 1;
    memcpy(segPlate, p, len);
    segPlate[len] = '\0';

    /* 段2: 置信度 */
    p     = comma + 1;
    comma = strchr(p, ',');
    len   = (comma != NULL) ? (uint16_t)(comma - p) : (uint16_t)strlen(p);
    if (len >= sizeof(segConf)) len = sizeof(segConf) - 1;
    memcpy(segConf, p, len);
    segConf[len] = '\0';

    /* 段3: 帧号(仅排障) */
    if (comma != NULL)
        g_plateCache.frameNo = (uint32_t)strtoul(comma + 1, NULL, 10);
    else
        g_plateCache.frameNo = 0;

    /* 填车牌/有效性 */
    if (segPlate[0] == '\0' || strcmp(segPlate, "-") == 0)
    {
        strcpy(g_plateCache.plate, "-");
        g_plateCache.valid = 0;
        g_plateCache.conf  = 0;         /* 未识别置信度恒 0(平台规则) */
    }
    else
    {
        long c;
        strncpy(g_plateCache.plate, segPlate, sizeof(g_plateCache.plate) - 1);
        g_plateCache.plate[sizeof(g_plateCache.plate) - 1] = '\0';
        g_plateCache.valid = 1;
        c = strtol(segConf, NULL, 10);
        if (c < 0)   c = 0;
        if (c > 100) c = 100;
        g_plateCache.conf = (uint8_t)c;
    }

    g_plateCache.source  = s_captureSrc;
    g_plateCache.color   = 0;           /* 一期恒 0 */
    g_plateCache.hasData = 1;
}

/****************************************************************************
 * S5: hex 字符 -> 数值, 非法返回 -1
 ****************************************************************************/
static int Plate_HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/****************************************************************************
 * S5: 缩略图传输行处理(全局分派, 与拍照/探活状态无关)
 *   $IMGD,<len>,<imgNo>  开收: 旧图立即作废, 只收 282B 标准图
 *   $IMGB,<hex>          数据行: 解到 g_thumbCache, 半截/脏 hex 直接放弃本图
 *   $IMGE,<crc16>        结束行: 长度+CRC16 双校验, 过了才提交 (置 valid/bits)
 *   直接写 g_thumbCache: 本图收不完时 valid 已在 $IMGD 置 0, 残缺数据不会被当图用
 ****************************************************************************/
static void Plate_OnThumbLine(const char *line)
{
    if (strncmp(line, "$IMGD,", 6) == 0)
    {
        const char *comma = strchr(line + 6, ',');
        long len = strtol(line + 6, NULL, 10);
        if (comma == NULL || len != (long)LORA_IMG_BYTES)
        {
            s_rxOn = 0;
            Usart_Printf(USART_DEBUG, "[PLATE] 缩略图起始行异常(len=%ld), 忽略\r\n", len);
            return;
        }
        g_thumbValid = 0;                /* 旧图作废; 收完并通过校验才恢复 */
        s_rxLen = (uint16_t)len;
        s_rxPos = 0;
        s_rxNo  = (uint16_t)strtoul(comma + 1, NULL, 10);
        s_rxOn  = 1;
        return;
    }

    if (strncmp(line, "$IMGB,", 6) == 0)
    {
        const char *p = line + 6;
        if (!s_rxOn)
            return;
        while (p[0] != '\0' && p[1] != '\0')
        {
            int hi, lo;
            if (s_rxPos >= s_rxLen)      /* 超长: 脏数据, 放弃本图 */
            {
                s_rxOn = 0;
                return;
            }
            hi = Plate_HexVal(p[0]);
            lo = Plate_HexVal(p[1]);
            if (hi < 0 || lo < 0)
            {
                s_rxOn = 0;
                Usart_Printf(USART_DEBUG, "[PLATE] 缩略图数据行含非法hex, 放弃本图\r\n");
                return;
            }
            g_thumbCache[s_rxPos++] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        return;
    }

    if (strncmp(line, "$IMGE,", 6) == 0)
    {
        uint16_t rx, calc;
        if (!s_rxOn)
            return;
        s_rxOn = 0;
        if (s_rxPos != s_rxLen)
        {
            Usart_Printf(USART_DEBUG, "[PLATE] 缩略图不完整(%u/%u), 丢弃\r\n",
                         (unsigned)s_rxPos, (unsigned)s_rxLen);
            return;
        }
        calc = lora_crc16(g_thumbCache, s_rxLen);
        rx   = (uint16_t)strtoul(line + 6, NULL, 16);
        if (calc != rx)
        {
            Usart_Printf(USART_DEBUG, "[PLATE] 缩略图CRC错(收%04X 算%04X), 丢弃\r\n",
                         (unsigned)rx, (unsigned)calc);
            return;
        }
        g_thumbNo       = s_rxNo;
        g_thumbValid    = 1;
        g_imgFetchPending = 1;           /* 立 CamFlags bit4, 网关择机 AT+IMG 取 */
        s_thumbReq      = 0;
        if (s_state == PC_THUMB_SENT)
        {
            s_state     = PC_IDLE;
            s_idleSince = Get_Tick();
        }
        Usart_Printf(USART_DEBUG, "[PLATE] 缩略图收妥: imgNo=%u %uB crc=%04X\r\n",
                     (unsigned)g_thumbNo, (unsigned)s_rxLen, (unsigned)calc);
        return;
    }
}

/****************************************************************************
 * 单行处理: 按当前状态分派
 ****************************************************************************/
static void Plate_OnLine(const char *line)
{
    /* S5: 缩略图传输行($IMGD/$IMGB/$IMGE)全局分派 —— 必须在状态机之前:
     * 取图窗口(PC_THUMB_SENT)收到的这些行与拍照/探活状态无关, 任何状态下都先吃掉 */
    if (line[0] == '$' &&
        (strncmp(line, "$IMGD,", 6) == 0 ||
         strncmp(line, "$IMGB,", 6) == 0 ||
         strncmp(line, "$IMGE,", 6) == 0))
    {
        Plate_OnThumbLine(line);
        return;
    }

    /* 探活态: 收到任何一行即证明摄像头在线 */
    if (s_state == PC_PROBE_SENT)
    {
        if (!g_camOnline)
            Usart_Printf(USART_DEBUG, "[PLATE] 摄像头在线\r\n");
        g_camOnline = 1;
        s_probeFail = 0;
        s_state     = PC_IDLE;
        s_idleSince = Get_Tick();
        return;
    }

    /* S5: 取图窗口收到 ERR(摄像头不认识 AT+THUMB / 还没有图) —— 必须打出来,
     * 否则被下面的"非拍照窗口静默丢弃"吞掉, 现场只看得到干巴巴的超时 */
    if (s_state == PC_THUMB_SENT && strncmp(line, "ERR:", 4) == 0)
    {
        Usart_Printf(USART_DEBUG, "[PLATE] 取缩略图被拒: %s\r\n", line);
        return;
    }

    /* 非拍照窗口的行(如摄像头 TRIG=0 主动发的行): 丢弃 */
    if (s_state != PC_RUN_SENT)
        return;

    /* $PLATE 结果行: 只认本次窗口首行 */
    if (strncmp(line, "$PLATE,", 7) == 0)
    {
        if (s_runGotResult)
        {
            s_strayPlate++;
            return;
        }
        s_runGotResult      = 1;
        Plate_ParseResult(line);
        g_plateFetchPending = 1;
        /* S5: 识别出有效车牌才接着取缩略图(无效帧没图可看, 省一次图传输) */
        s_thumbReq          = g_plateCache.valid ? 1 : 0;
        s_captureReq        = 0;
        s_state             = PC_IDLE;
        s_idleSince         = Get_Tick();
        Usart_Printf(USART_DEBUG, "[PLATE] 结果: %s conf=%d valid=%d frameNo=%lu\r\n",
                     g_plateCache.plate, g_plateCache.conf, g_plateCache.valid,
                     (unsigned long)g_plateCache.frameNo);
        return;
    }

    /* 被抢占: ERR: 行, 不算"未识别", 不许当 '-' 上报 */
    if (strncmp(line, "ERR:", 4) == 0)
    {
        s_camPreempted++;
        s_captureReq = 0;
        s_state      = PC_IDLE;
        s_idleSince  = Get_Tick();
        Usart_Printf(USART_DEBUG, "[PLATE] 被抢占: %s (累计%d次)\r\n",
                     line, s_camPreempted);
        return;
    }

    /* +INFO: / +CFG: / OK 等: 丢弃, 继续等 $PLATE */
}

/* ==================== 接口函数实现 ==================== */

void Plate_Init(void)
{
    Usart3_Init(PLATE_BAUD);

    memset(&g_plateCache, 0, sizeof(g_plateCache));
    strcpy(g_plateCache.plate, "-");
    g_capturePolicy     = CAPTURE_POLICY_DEFAULT;
    g_plateFetchPending = 0;
    g_camOnline         = 0;
    memset(g_thumbCache, 0, sizeof(g_thumbCache));   /* S5 */
    g_thumbValid        = 0;
    g_imgFetchPending   = 0;
    g_thumbNo           = 0;

    s_state          = PC_IDLE;
    s_thumbReq       = 0;
    s_rxOn           = 0;
    s_bootAt         = Get_Tick();
    s_idleSince      = s_bootAt;
    /* 首次探活: 把上次探活时刻回拨一个周期, 使静默期(PLATE_ARM_DELAY_MS)
     * 一过、通道一空闲就立即发 AT+INFO, 尽早上线摄像头在线状态,
     * 不必干等一个 PLATE_PROBE_MS 周期 */
    s_lastProbeAt    = s_bootAt - PLATE_PROBE_MS;
    s_prevStatus     = PARK_IDLE;
    s_lastAutoAt     = s_bootAt - PLATE_COOLDOWN_MS;   /* 首次触发不受冷却限制 */
    s_captureReq     = 0;
    s_autoArmed      = 0;
    s_probeFail      = 0;
    s_lineLen        = 0;
    s_lineDiscard    = 0;

    Usart_Printf(USART_DEBUG,
                 "[PLATE] 车牌子系统初始化 (USART3 115200, 策略=%d, 静默%lums)\r\n",
                 g_capturePolicy, (unsigned long)PLATE_ARM_DELAY_MS);
}

void Plate_RequestCapture(uint8_t src)
{
    s_captureReq = 1;
    s_captureSrc = src;
    /* 不变量: 正在等 AT+INFO 回复时不打断, 等本轮探活收尾后由 PC_IDLE 发出 */
}

void Plate_Task(void)
{
    uint8_t  buf[64];
    uint16_t n;
    uint32_t now;

    /* 取数据(非阻塞), 逐行喂状态机 */
    while ((n = Usart3_GetData(buf, sizeof(buf))) > 0)
        Plate_FeedBytes(buf, n);

    /* 自动触发判定(只置请求旗子, 不直接发命令) */
    Plate_CheckAutoTrigger();

    /* 查超时 / 推进状态机 */
    now = Get_Tick();
    switch (s_state)
    {
        case PC_RUN_SENT:
            if (s_awaitRetry)
            {
                if ((uint32_t)(now - s_retryAt) >= PLATE_RETRY_GAP_MS)
                {
                    Usart3_FlushRx();
                    Usart3_SendAsync((const uint8_t *)PLATE_CMD_RUN,
                                     sizeof(PLATE_CMD_RUN) - 1);
                    s_sentAt       = Get_Tick();
                    s_awaitRetry   = 0;
                    s_runGotResult = 0;
                    Usart_Printf(USART_DEBUG, "[PLATE] -> AT+RUN 重试#%d\r\n",
                                 s_retryCnt);
                }
            }
            else if ((uint32_t)(now - s_sentAt) >= PLATE_TIMEOUT_MS)
            {
                if (s_retryCnt < PLATE_RETRY_MAX)
                {
                    s_retryCnt++;
                    s_awaitRetry = 1;
                    s_retryAt    = now;
                    Usart_Printf(USART_DEBUG,
                                 "[PLATE] AT+RUN 超时, %lums 后重试#%d\r\n",
                                 (unsigned long)PLATE_RETRY_GAP_MS, s_retryCnt);
                }
                else
                {
                    s_camTimeout++;
                    Usart_Printf(USART_DEBUG,
                                 "[PLATE][ERR] 拍照超时放弃(累计%d次)\r\n", s_camTimeout);
                    s_captureReq = 0;
                    s_state      = PC_IDLE;
                    s_idleSince  = now;
                }
            }
            break;

        case PC_PROBE_SENT:
            if ((uint32_t)(now - s_sentAt) >= PLATE_PROBE_TIMEOUT_MS)
            {
                s_probeFail++;
                if (s_probeFail >= PLATE_PROBE_FAIL_MAX)
                {
                    if (g_camOnline)
                        Usart_Printf(USART_DEBUG,
                                     "[PLATE] 摄像头离线(探活连续%d次无回应)\r\n",
                                     s_probeFail);
                    g_camOnline = 0;
                }
                s_state     = PC_IDLE;
                s_idleSince = now;
            }
            break;

        case PC_THUMB_SENT:
            /* S5: $IMGD..$IMGE 没在 1.5s 内收完(丢行/摄像头没理) -> 放弃本次取图。
             * 不重试: 下次识别出有效车牌自然再来一轮; 残留半截传输由 s_rxOn 收口 */
            if ((uint32_t)(now - s_sentAt) >= PLATE_THUMB_TIMEOUT_MS)
            {
                s_rxOn     = 0;
                s_thumbReq = 0;
                Usart_Printf(USART_DEBUG, "[PLATE] 取缩略图超时, 放弃本次\r\n");
                s_state     = PC_IDLE;
                s_idleSince = now;
            }
            break;

        case PC_IDLE:
        default:
            Plate_TryNextCommand();
            break;
    }
}

const LoraPlate_t *Plate_BuildFrame(void)
{
    static LoraPlate_t frame;

    memset(&frame, 0, sizeof(frame));
    strncpy(frame.plate, g_plateCache.plate, sizeof(frame.plate) - 1);
    frame.conf    = g_plateCache.conf;
    frame.valid   = g_plateCache.valid;
    frame.source  = g_plateCache.source;
    frame.frameNo = g_plateCache.frameNo;
    frame.color   = g_plateCache.color;
    /* crc16 由 LoRa_Node_SendPlate 统一计算 */

    return &frame;
}
