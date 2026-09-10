/****************************************************************************
 * BootLoader 主程序 - main.c
 *
 * 功能描述:
 *   实现节点端 BootLoader 功能：
 *   - 支持基于 LoRa 的远程固件升级(OTA)
 *   - 启动时检查升级标志/按键, 决定进入 Boot 或跳转到 APP
 *
 * 通信协议:
 *   采用 Xmodem 协议, 通过 LoRa 模块(USART2)接收固件数据
 *   每包 128B + CRC16 校验 + ACK/NAK 应答, 超时 120s
 *
 * 硬件平台: STM32F103C8T6
 *   USART2(PA2-TX, PA3-RX) - LoRa 模块
 *   PA0  - FLASH 按键(用于进入Boot)
 *   IWDG - 独立看门狗, 4s 超时
 *
 * 启动流程: 0-BootLoader等待窗口 → Load_APP
 ****************************************************************************/

#include <string.h>
#include <stddef.h>   /* offsetof */
#include "boot.h"
#include "boot_flash.h"   /* ⭐ 统一 Flash 标志页接口 (S8): Flash_ReadOtaFlag 等 */
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"
#include "stm32f10x_iwdg.h"
#include "stm32f10x_flash.h"
#include "stm32f10x_exti.h"
#include "misc.h"

/* 应用入口函数指针类型 */
typedef void (*pFunction)(void);

/* ==================== 全局变量 ==================== */
volatile uint32_t g_bootTick = 0;

/* ⭐ S26: 期待的下一个新包序号(网关从1开始, 每包+1).
 * 由 OTA_Process 局部变量提升为文件级, 供 OTA_ReceivePacket 在
 * 擦除等待窗口内收到完整 AT+OTA 触发命令时重置为 1 —— 网关每次
 * 整链重试/自愈重发都以 AT+OTA 为起点、之后必从 #1 发包, 节点
 * 只有在此刻把 expectSeq 对齐回 1, 才不会残留上一轮失败序号
 * (历史现象: expect=57 recv=1 → 永久序号错位 NAK 死循环) */
static uint16_t otaExpectSeq = 1;

/* 接收状态机 */
typedef enum {
    RX_STATE_WAIT_SOH,      /* 等待 SOH(0x02) */
    RX_STATE_WAIT_SEQ_H,    /* 等待序号高字节 */
    RX_STATE_WAIT_SEQ_L,    /* 等待序号低字节 */
    RX_STATE_WAIT_DATA,     /* 接收 128B 数据 */
    RX_STATE_WAIT_CRC_H,    /* 等待 CRC16 高字节 */
    RX_STATE_WAIT_CRC_L,    /* 等待 CRC16 低字节 */
    RX_STATE_DONE,          /* 接收完成 */
} RxState_t;

/* ==================== 内部函数声明 ==================== */
static void SysTick_Init(void);
static void USART2_Init(uint32_t baud);
static void IWDG_Init(uint16_t reload);
static void Key_Init(void);
static uint8_t Key_IsPressed(void);
static void USART2_SendByte(uint8_t b);
static uint8_t USART2_ReadByte(uint8_t *b);
static void OTA_SendAckString(const char *s);
static uint8_t OTA_CollectAtCmd(void);
static void OTA_HandleTriggerCmd(void);
static uint8_t OTA_ReceivePacket(uint8_t *data, uint16_t *seq, uint16_t *crc,
                                 uint32_t fwLen, uint32_t totalWrote);
static void OTA_Process(void);
static void Boot_Delay(uint32_t ms);
static void USART1_PutString(const char *s);
static void USART1_PutChar(char c);
static void USART1_PutUInt(uint32_t val);
static void USART1_PutHex(uint32_t val);

/* ==================== 系统时钟 ==================== */

void SysTick_Handler(void)
{
    g_bootTick++;
}

static void SysTick_Init(void)
{
    /* 配置 SysTick 为 72MHz, 1ms 中断 */
    if (SysTick_Config(SystemCoreClock / 1000))
        while (1);
}

uint32_t Boot_GetTick(void)
{
    return g_bootTick;
}

/* ==================== USART2 (LoRa 模块) ==================== */

static void USART2_Init(uint32_t baud)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);

    /* PA2 TX */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_2;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* PA3 RX */
    gpio.GPIO_Pin = GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    /* USART 配置 */
    USART_StructInit(&usart);
    usart.USART_BaudRate = baud;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART2, &usart);
    USART_Cmd(USART2, ENABLE);
}

static void USART2_SendByte(uint8_t b)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET);
    USART_SendData(USART2, b);
}

static uint8_t USART2_ReadByte(uint8_t *b)
{
    if (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) == RESET)
        return 0;
    *b = (uint8_t)USART_ReceiveData(USART2);
    return 1;
}

/* 发送 OTA 应答包(ACK/NAK/EOT/CAN)
 * 注意: 需要添加 [AddrH][AddrL][CH] 定点传输帧头
 * 因为 LoRa 模块处于定点传输模式, 需要目标地址
 * 参数: b - 应答字节(ACK/NAK/EOT/CAN) */
static void OTA_SendResp(uint8_t b)
{
    /* 添加目标地址帧头 */
    USART2_SendByte((uint8_t)(LORA_GATEWAY_ADDR >> 8));     /* 网关地址高字节 */
    USART2_SendByte((uint8_t)(LORA_GATEWAY_ADDR & 0xFF));   /* 网关地址低字节 */
    USART2_SendByte(LORA_CHANNEL);                          /* 信道 */
    USART2_SendByte(b);                                     /* 应答字节 */
}

/* 发送字符串 ACK 帧 (AT+OTA 触发确认)
 * 帧格式: [AddrH][AddrL][CH][LORA_FRAME_ACK][s][\r][\n]
 * 与 App 端 bsp_lora.c 的 LoRa_Node_SendAck 完全一致:
 * 网关 LORA_FRAME_ACK 分支按 "AT+OTA" 前缀匹配, 置位 s_triggerAcked.
 * 注意: 不能用裸 OTA_SendResp(ACK) 代替——0x03 在网关 RX_WAIT_HEADER
 * 会被喂给 ota_feedByte, 而 ota_feedByte 在触发状态不处理, 无法确认送达 */
static void OTA_SendAckString(const char *s)
{
    USART2_SendByte((uint8_t)(LORA_GATEWAY_ADDR >> 8));     /* 网关地址高字节 */
    USART2_SendByte((uint8_t)(LORA_GATEWAY_ADDR & 0xFF));   /* 网关地址低字节 */
    USART2_SendByte(LORA_CHANNEL);                          /* 信道 */
    USART2_SendByte(LORA_FRAME_ACK);                        /* 字符串ACK帧头 */
    while (*s)
        USART2_SendByte((uint8_t)*s++);
    USART2_SendByte('\r');
    USART2_SendByte('\n');
}

/* ==================== IWDG 看门狗 ==================== */

static void IWDG_Init(uint16_t reload)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);   /* LSI 40kHz / 64 = 625Hz */
    IWDG_SetReload(reload);                 /* 2500 / 625 = 4s 超时 */
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void Boot_FeedWatchdog(void)
{
    IWDG_ReloadCounter();
}

/* ==================== 按键(PA0 FLASH) ==================== */

static void Key_Init(void)
{
    GPIO_InitTypeDef gpio;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_0;
    gpio.GPIO_Mode = GPIO_Mode_IPU;         /* 上拉输入 */
    GPIO_Init(GPIOA, &gpio);
}

static uint8_t Key_IsPressed(void)
{
    /* PA0 低电平表示按下(FLASH 按键) */
    return (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0) == 0);
}

/* ==================== 延时 ==================== */

static void Boot_Delay(uint32_t ms)
{
    uint32_t start = Boot_GetTick();
    while (Boot_GetTick() - start < ms)
    {
        Boot_FeedWatchdog();                /* 喂狗, 防止复位 */
    }
}

/* ==================== Flash 操作 ====================
 * ⭐ 升级标志页(GO/DONE + APP完整性)读写统一走共享头 boot_flash.h (S8):
 *   Flash_ReadOtaFlag / Flash_ReadAppPartial / Flash_SaveOtaFlag /
 *   Flash_SaveOtaFlagKeepPartial / Flash_SetAppPartial
 *   此处仅保留 APP 区擦除/写入 */

uint8_t OTA_EraseAppArea(void)
{
    uint32_t addr;
    FLASH_Unlock();
    for (addr = APP_ADDR; addr < APP_ADDR + APP_MAX_SIZE; addr += 1024)
    {
        FLASH_ErasePage(addr);
        Boot_FeedWatchdog();                /* 擦除期间喂狗, 防止复位 */
    }
    FLASH_Lock();
    return 1;
}

uint8_t OTA_FlashWrite(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint16_t halfWord;

    FLASH_Unlock();
    for (i = 0; i < len; i += 2)
    {
        halfWord = data[i];
        if (i + 1 < len)
            halfWord |= (uint16_t)data[i + 1] << 8;

        /* ⭐ 编程前清错误标志 (PGERR/WRPRTERR/EOP):
         * STM32 硬件语义 —— PGERR 一旦置位, 后续所有编程操作持续被阻断,
         * 必须 FLASH_ClearFlag 才能继续写; 否则一次写失败 = 本次会话永久写失败
         * (历史根因: "无法擦写"后每一轮整链重试都在同一地址连续失败) */
        FLASH_ClearFlag(FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR | FLASH_FLAG_EOP);

        if (FLASH_ProgramHalfWord(addr + i, halfWord) != FLASH_COMPLETE)
        {
            /* ⭐ 失败清标志重试一次 (防瞬时错误标志/上电残留干扰), 仍失败才报错 */
            FLASH_ClearFlag(FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR | FLASH_FLAG_EOP);
            if (FLASH_ProgramHalfWord(addr + i, halfWord) != FLASH_COMPLETE)
            {
                FLASH_Lock();
                return 0;
            }
        }

        /* 读回验证 */
        if (*(volatile uint16_t *)(addr + i) != halfWord)
        {
            FLASH_Lock();
            return 0;
        }

        /* 每写 512 字节喂一次狗 */
        if ((i & 0x1FF) == 0)
            Boot_FeedWatchdog();
    }

    FLASH_Lock();
    return 1;
}

/* ==================== CRC16-XMODEM ==================== */

uint16_t CRC16_XMODEM(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0;
    uint32_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ==================== 软件 CRC32 ==================== */

uint32_t CRC32_Soft(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    uint32_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/* ==================== 节点身份配置区 ====================
 * 首次启动时把本节点身份(NODE_PRODUCT_KEY/NODE_DEVICE_NAME)
 * 固化到 Flash 配置区(0x08003000). OTA 只擦写 APP 区, 配置区不受影响,
 * 因此各节点可共用同一份纯净的 OTA 固件包, 身份各自保留.
 * 空中寻址用 LoRa 模块硬件地址(人工配置), 配置区不再存节点地址. */

static void NodeConfig_Init(void)
{
    const NodeConfig_t *src = (const NodeConfig_t *)NODE_CONFIG_ADDR;
    uint32_t i, addr;
    uint16_t halfWord;

    /* 配置区有效(magic+CRC32)则跳过, 后续重启/OTA 不再写 */
    if (src->magic == NODE_CONFIG_MAGIC &&
        CRC32_Soft((const uint8_t *)src, offsetof(NodeConfig_t, crc32)) == src->crc32)
    {
        USART1_PutString("[CFG] 节点配置有效: 产品=");
        USART1_PutString(src->productKey);
        USART1_PutString(" 设备=");
        USART1_PutString(src->deviceName);
        USART1_PutString("\r\n");
        return;
    }

    /* 首次启动/配置无效: 用编译期宏落盘固化本节点身份 */
    {
        NodeConfig_t cfg;
        const uint8_t *p;

        memset(&cfg, 0, sizeof(cfg));
        cfg.magic = NODE_CONFIG_MAGIC;
        strncpy(cfg.deviceName, NODE_DEVICE_NAME, sizeof(cfg.deviceName) - 1);
        strncpy(cfg.productKey, NODE_PRODUCT_KEY, sizeof(cfg.productKey) - 1);
        p = (const uint8_t *)&cfg;
        cfg.crc32 = CRC32_Soft(p, offsetof(NodeConfig_t, crc32));

        FLASH_Unlock();
        FLASH_ErasePage(NODE_CONFIG_ADDR);
        for (i = 0, addr = NODE_CONFIG_ADDR; i < sizeof(NodeConfig_t); i += 2)
        {
            halfWord = p[i];
            if (i + 1 < sizeof(NodeConfig_t))
                halfWord |= (uint16_t)p[i + 1] << 8;
            if (FLASH_ProgramHalfWord(addr + i, halfWord) != FLASH_COMPLETE)
            {
                FLASH_Lock();
                USART1_PutString("[CFG][ERR] 节点身份写入Flash失败\r\n");
                return;
            }
        }
        FLASH_Lock();

        USART1_PutString("[CFG] 首次启动, 节点身份已写入Flash: 产品=");
        USART1_PutString(cfg.productKey);
        USART1_PutString(" 设备=");
        USART1_PutString(cfg.deviceName);
        USART1_PutString("\r\n");
    }
}

/* ==================== 跳转到 APP ==================== */

void Load_APP(uint32_t appxaddr)
{
    uint32_t stackTop;
    pFunction jump;

    /* 检查 APP 栈顶地址是否有效 */
    stackTop = *(volatile uint32_t *)appxaddr;
    if ((stackTop & 0x2FFE0000) != 0x20000000)
        return;                             /* 无效, 停留在 Boot */

    /* 关闭全局中断 */
    __disable_irq();

    /* 关闭 SysTick 定时器(防止中断跳转到不存在的向量表) */
    SysTick->CTRL = 0;

    /* 设置 MSP 为 APP 的栈顶 */
    __set_MSP(stackTop);

    /* 跳转到 APP 的 Reset_Handler */
    jump = (pFunction)(*(volatile uint32_t *)(appxaddr + 4));
    jump();
}

/* ==================== AT 字节级匹配器 (S22 单一实现) ====================
 * OTA_CollectAtCmd (空闲/救援循环) 与 OTA_ReceivePacket WAIT_SOH 分支
 * (擦除等待窗口) 共用同一前缀匹配状态机, 避免双实现漂移 (S6).
 * 逐字节喂入, 行结束(\r/\n)判定完整命令:
 *   返回 0 = 无完整命令; 1 = 完整 AT+OTA; 2 = 完整 AT+PING */
static uint8_t OTA_FeedAtByte(uint8_t b)
{
    static uint8_t atLen = 0;
    static uint8_t atMatched = 0;   /* 0=未匹配, 1=AT+OTA, 2=AT+PING */

    if (b == '\r' || b == '\n')
    {
        uint8_t got = atMatched;
        atLen = 0;
        atMatched = 0;
        return got;
    }

    if (!atMatched)
    {
        /* 未匹配到前缀: 匹配 "AT+OTA"(6字节) 或 "AT+PING"(7字节),
         * atLen 即已匹配的前缀字符数 (逐字节比较, 无需缓冲) */
        if (atLen < 6 && b == (uint8_t)"AT+OTA"[atLen])
        {
            atLen++;
            if (atLen == 6)
                atMatched = 1;
        }
        else if (atLen < 7 && b == (uint8_t)"AT+PING"[atLen])
        {
            atLen++;
            if (atLen == 7)
                atMatched = 2;
        }
        else
        {
            atLen = 0;  /* 前缀不符, 重新开始 */
        }
    }
    /* atMatched 前缀后的参数部分不解析, 忽略直到 \r\n 判定命令结束 */
    return 0;
}

/* ==================== Xmodem 包接收 ==================== */

static uint8_t OTA_ReceivePacket(uint8_t *data, uint16_t *seq, uint16_t *crc,
                                 uint32_t fwLen, uint32_t totalWrote)
{
    RxState_t state = RX_STATE_WAIT_SOH;
    uint8_t b;
    uint16_t idx = 0;
    uint32_t timeout = Boot_GetTick();
    uint8_t bufSeqH = 0, bufSeqL = 0;
    uint8_t bufCrcH = 0, bufCrcL = 0;
    static uint8_t canSeen = 0; /* ⭐ 双CAN确认: 已收到第一个CAN (跨调用保持) */
    static uint8_t eotSeen = 0; /* ⭐ S26 双EOT确认: 已收到第一个EOT (跨调用保持) */

    while ((Boot_GetTick() - timeout) < OTA_PACKET_TIMEOUT_MS)
    {
        if (USART2_ReadByte(&b))
        {
            timeout = Boot_GetTick();       /* 收到数据, 重置超时 */

            switch (state)
            {
                case RX_STATE_WAIT_SOH:
                    if (b == OTA_SOH)
                    {
                        canSeen = 0;        /* ⭐ 新传输开始, 清双CAN确认 */
                        eotSeen = 0;        /* ⭐ S26: 新传输开始, 清双EOT确认 */
                        state = RX_STATE_WAIT_SEQ_H;
                        idx = 0;
                    }
                    else if (b == OTA_EOT)
                    {
                        /* ⭐ S26 EOT 双确认 + 语义校验 (标准依据见下).
                         * [第一层·语义校验] Xmodem 语义: EOT=发送方已发完所有数据.
                         * 节点从固件头已知总长 fwLen, 只有写入量 totalWrote 达到
                         * fwLen-12 时才可能存在真 EOT; 未收满前收到 0x04 必为
                         * 空中杂波/残留字节, 直接忽略(连单字节确认都不置位).
                         * 从语义上杜绝"14/36包即结束"(网关日志从未真发过 EOT).
                         * [第二层·双字节确认] 对齐 Xmodem 规范对不可靠链路的多字节
                         * 控制符惯例(取消须连发8个CAN)与本项目已有双CAN确认: 收满后
                         * 仍需连续两个 0x04 才判结束, 防收满瞬间单字节杂波误掐.
                         * 期间收到 SOH 则取消(网关仍在发包, 说明未结束) */
                        if (fwLen == 0 ||
                            totalWrote < (uint32_t)(fwLen - sizeof(FwHeader_t)))
                            break;          /* 数据未收满, 不可能是真EOT, 忽略 */
                        if (eotSeen)
                        {
                            eotSeen = 0;
                            canSeen = 0;
                            return 2;       /* 传输结束 */
                        }
                        eotSeen = 1;
                    }
                    else if (b == OTA_CAN)
                    {
                        /* ⭐ 双CAN确认 (Xmodem 规范): 必须连续收到两个 CAN 才取消,
                         * 防空口误码/残留帧单字节撞 0x18 误中断升级 (历史现象:
                         * 用户拔线瞬间节点误收 CAN, 正常升级被掐断进入恢复态) */
                        if (canSeen)
                        {
                            canSeen = 0;
                            eotSeen = 0;
                            return 3;
                        }
                        canSeen = 1;
                    }
                    else
                    {
                        /* ⭐ S22: 擦除等待窗口内顺带响应 AT 命令 (修复自愈真空窗).
                         * 节点在 OTA_Process 的 180s 窗口内只认 Xmodem 字节,
                         * 网关的 AT+PING/AT+OTA 被当垃圾丢弃 → 不回 PONG →
                         * 网关判定器不触发 → 自愈被锁死到总超时(3分钟).
                         * 此处把非协议字节喂给统一 AT 匹配器:
                         *   - AT+PING → 回 PONG,BOOT (网关判定器立即触发,
                         *               整链重来自愈, 不再等 180s)
                         *   - AT+OTA  → 回 AT+OTA:ack (网关从触发阶段直接
                         *               进入发数据阶段, 节点已在此等 SOH,
                         *               无缝衔接. 不写GO不复位, 保持两阶段
                         *               提交"先验头后擦除"语义) */
                        uint8_t at = OTA_FeedAtByte(b);
                        if (at == 1)
                        {
                            USART1_PutString("[BOOT] 窗口内收到AT+OTA, 回ACK等待固件\r\n");
                            /* ⭐ S26: 网关整链重试/自愈均以 AT+OTA 为起点,
                             * 之后必从 #1 发包. 此处必须把期待序号对齐回 1,
                             * 否则残留上一轮失败序号 (如 57) → 网关发 #1 被
                             * 判"序号错位"回 NAK → 永久死循环 (历史现象) */
                            otaExpectSeq = 1;
                            canSeen = 0;
                            eotSeen = 0;
                            OTA_SendAckString("AT+OTA:ack");
                        }
                        else if (at == 2)
                        {
                            OTA_SendAckString("PONG,BOOT");
                        }
                    }
                    break;

                case RX_STATE_WAIT_SEQ_H:
                    bufSeqH = b;
                    state = RX_STATE_WAIT_SEQ_L;
                    break;

                case RX_STATE_WAIT_SEQ_L:
                    bufSeqL = b;
                    state = RX_STATE_WAIT_DATA;
                    break;

                case RX_STATE_WAIT_DATA:
                    data[idx++] = b;
                    if (idx >= OTA_PACKET_DATA_SIZE)
                        state = RX_STATE_WAIT_CRC_H;
                    break;

                case RX_STATE_WAIT_CRC_H:
                    bufCrcH = b;
                    state = RX_STATE_WAIT_CRC_L;
                    break;

                case RX_STATE_WAIT_CRC_L:
                    bufCrcL = b;
                    *seq = ((uint16_t)bufSeqH << 8) | bufSeqL;
                    *crc = ((uint16_t)bufCrcH << 8) | bufCrcL;
                    return 1;               /* 接收成功 */

                default:
                    break;
            }
        }

        Boot_FeedWatchdog();
    }

    return 0;                               /* 超时 */
}

/* ==================== OTA 失败统一恢复 (P0-B/P0-C) ====================
 * 所有升级失败路径统一入口:
 *   1. 清除升级标志(写 OTA_FLAG_DONE), 防止下次复位重复擦 APP 死循环
 *      (原缺陷: 失败不清标志 → 看门狗/上电复位反复"擦APP→空等" = 节点死亡)
 *   2. 回复 NAK 通知网关
 *   3. 两阶段提交下: 若 APP 区未被擦除(appErased==0), 旧 APP 完好,
 *      直接 Load_APP 瞬时恢复(升级前工作状态);
 *      若已擦除(appErased==1), Load_APP 栈顶校验失败返回, 节点保持
 *      Boot 空闲等待重新升级(安全等待态, 非死亡) */
static void OTA_AbortRecovery(uint8_t appErased)
{
    Flash_SaveOtaFlagKeepPartial(OTA_FLAG_DONE);
    OTA_SendResp(OTA_NAK);

    if (!appErased)
    {
        USART1_PutString("[BOOT] APP区未损坏, 回退启动旧APP\r\n");
        Flash_SetAppPartial(0);     /* 一致性: 回退前 APP 区完好 */
        Load_APP(APP_ADDR);
        return;                             /* Load_APP 成功跳转不返回, 仅防御 */
    }

    USART1_PutString("[BOOT] APP区已擦除, 保持Boot等待重新升级\r\n");
}

/* ==================== AT 命令收集/处理 (触发 + 保活) ====================
 * ⭐ 规范 AT 指令替代原裸字符 'O' (零协议):
 * 网关触发命令格式 "AT+OTA=start,<ver>\r\n", 本函数逐字节收集,
 * 前缀匹配 "AT+OTA" 后收参数直到 \r/\n 判定为一条完整命令.
 * ⭐ Boot 通信模式(仅保活应答): 并行匹配 "AT+PING" 前缀, 收到完整
 * 指令后回 "PONG,BOOT", 让网关判定节点在线且处于 Boot 模式; 其余业务命令一律丢弃.
 * ⭐ S6: 单一 AT 收集实现 (OTA_ReceivePacket 内已删内嵌收集, 统一走本函数) */

static uint8_t OTA_CollectAtCmd(void)
{
    uint8_t b;

    while (USART2_ReadByte(&b))
    {
        uint8_t got = OTA_FeedAtByte(b);
        if (got == 1)
            return 1;           /* 完整 AT+OTA 命令 */
        if (got == 2)
        {
            /* ⭐ Boot 保活: 网关 AT+PING 探活 → 回 PONG,BOOT, 保持节点在线
             * 并上报 Boot 模式 (方案 5.1) */
            OTA_SendAckString("PONG,BOOT");
        }
    }
    return 0;
}

/* ⭐ AT+OTA 触发命令处理 (等待循环/2s窗口/P0-D救活循环共用):
 * 回字符串ACK确认送达 → 写GO标志 → 复位进升级模式 */
static void OTA_HandleTriggerCmd(void)
{
    USART1_PutString("[BOOT] 收到AT+OTA命令, 进入升级模式\r\n");
    OTA_SendAckString("AT+OTA:ack");
    Flash_SaveOtaFlagKeepPartial(OTA_FLAG_GO);
    Boot_Delay(100);            /* 等串口发完再复位 */
    NVIC_SystemReset();
}

/* ==================== OTA 主流程 ==================== */

static void OTA_Process(void)
{
    uint8_t  pktBuf[OTA_PACKET_DATA_SIZE];
    uint16_t seq, crcRecv, crcCalc;
    uint8_t  ret;
    uint32_t totalWrote = 0;
    uint32_t startTick = Boot_GetTick();
    uint32_t fwLen = 0;
    uint8_t  fwHeader[sizeof(FwHeader_t)];
    uint8_t  headerDone = 0;
    uint8_t  otaFailed = 0;
    uint16_t pktCount = 0;
    uint8_t  appErased = 0;     /* ⭐ 两阶段提交: 本次运行是否已擦除 APP 区 */
    uint32_t lastNakTick = 0;   /* ⭐ 擦除后 NAK 催发计时 (每10s催一次) */
    uint8_t  nakUrgeCount = 0;  /* ⭐ NAK 催发计数: 网关连续无响应时停止催发, 防无限刷屏 */

    /* ⭐ S26: 每轮升级从 #1 开始期待 (Xmodem: 新传输从1重新编号).
     * 原局部变量 expectSeq 已提升为文件级 otaExpectSeq (供 OTA_ReceivePacket
     * 在擦除等待窗口内收到 AT+OTA 时同步重置=1); 此处仍保留入口重置, 保证
     * 每次进入 OTA_Process 的初始语义不变: 期待首包序号为 1 */
    otaExpectSeq = 1;

    /* ⭐ 断电续知: 断电重启后局部变量丢失, 从标志页读取"APP区是否已擦除/不完整".
     * 若上次擦除 APP 后断电, 此处借 appErased=1 让超时走 180s 总超时 + NAK 催发档位,
     * 等待网关整链重试重新触发/重发, 而非误判"旧APP完好"去回退坏固件.
     * ⚠ 注意: PARTIAL 仅表示"APP 区可能残留脏数据(非全0xFF)", 绝不代表"已擦干净":
     * 直接写入残留地址会触发 Flash PGERR → "无法擦写"卡死 (历史根因).
     * 首包固件头验证通过后仍会无条件重擦 (见下方两阶段提交执行点) */
    if (Flash_ReadAppPartial() == OTA_FLAG_APP_PARTIAL)
    {
        appErased = 1;
        USART1_PutString("[BOOT] 检测到APP区不完整(上次升级中断), 等待固件头后重新擦除\r\n");
    }

    USART1_PutString("[BOOT] 开始OTA升级, 等待固件...\r\n");

    /* ⭐ 两阶段提交(P0-C): 不再进入即擦除 APP 区.
     * 先等首包并验证固件头(magic/length), 验证通过后才擦除,
     * 保证"网关放弃/首包超时"时旧 APP 完好, 可瞬时回退 */

    /* 接收固件数据 */
    while (1)
    {
        /* ⭐ 超时检查: 首包阶段(未擦除)按 OTA_FIRST_PKT_TIMEOUT_MS 计时,
         * 超时即可安全回退旧APP; 擦除后按总超时 OTA_TOTAL_TIMEOUT_MS */
        if ((Boot_GetTick() - startTick) >=
            (appErased ? OTA_TOTAL_TIMEOUT_MS : OTA_FIRST_PKT_TIMEOUT_MS))
        {
            USART1_PutString("[BOOT] 超时, 已接收 ");
            USART1_PutUInt(pktCount);
            USART1_PutString(" 包 ");
            USART1_PutUInt(totalWrote);
            USART1_PutString("B\r\n");
            otaFailed = 1;
            break;
        }

        /* ⭐ 擦除后兜底(设计文档3.2.3): APP 已擦除且长时间无包,
         * 每 OTA_ERASED_NAK_INTERVAL_MS 主动发一次 NAK 催发网关,
         * 配合总超时 180s 等待网关整链重试重新发触发命令/数据包.
         * ⚠ 催发有上限: 网关连续 OTA_ERASED_NAK_MAX 次无响应(可能已放弃/
         * 删文件), 停止催发不再刷屏, 但仍持续接收数据包(网关重新触发可
         * 无缝恢复), 直到总超时回退 (历史现象: 网关删文件后节点无限"催促网关") */
        if (appErased && (Boot_GetTick() - lastNakTick) >= OTA_ERASED_NAK_INTERVAL_MS)
        {
            lastNakTick = Boot_GetTick();
            if (nakUrgeCount >= OTA_ERASED_NAK_MAX)
            {
                if (nakUrgeCount == OTA_ERASED_NAK_MAX)
                {
                    USART1_PutString("[BOOT] 网关长时间无响应, 停止催促 (等待重新触发/总超时回退)\r\n");
                    nakUrgeCount++;
                }
            }
            else
            {
                nakUrgeCount++;
                OTA_SendResp(OTA_NAK);
                USART1_PutString("[BOOT] APP已擦除, 发送NAK催促网关...\r\n");
            }
        }

        /* 接收数据包 */
        ret = OTA_ReceivePacket(pktBuf, &seq, &crcRecv, fwLen, totalWrote);

        if (ret == 1)
        {
            pktCount++;
            nakUrgeCount = 0;   /* ⭐ 收到有效数据包 → 网关有响应, 重置催发计数 */
            /* ⭐ S28: 收到有效数据包 → 数据流活跃, 顺延 NAK 催发定时.
             * (历史缺陷: 催发定时 lastNakTick 未随收包顺延 → 传输中途每 10s
             * 无脑发 NAK, 半双工下冲掉网关下行包 → #50 起 CRC16 校验失败) */
            lastNakTick = Boot_GetTick();
            /* 数据包接收成功, 验证 CRC16 */
            crcCalc = CRC16_XMODEM(pktBuf, OTA_PACKET_DATA_SIZE);

            if (crcCalc == crcRecv)
            {
                /* ⭐ S18: Xmodem seq 去重.
                 * 上次 ACK 丢失 → 网关超时/NAK 后重发同序包 → 直接回 ACK
                 * 不写 Flash, 避免重复写同地址触发 FLASH 编程错误/损坏数据.
                 * 非期望序号(乱序/错位)拒绝并要求重发, 防数据错位写盘 */
                if (seq != otaExpectSeq)
                {
                    if (seq == (uint16_t)(otaExpectSeq - 1))
                    {
                        USART1_PutString("[BOOT] 重复包 #");
                        USART1_PutUInt(seq);
                        USART1_PutString(", 回ACK不写Flash\r\n");
                        OTA_SendResp(OTA_ACK);
                        continue;
                    }
                    USART1_PutString("[BOOT] 序号错位 expect=");
                    USART1_PutUInt(otaExpectSeq);
                    USART1_PutString(" recv=");
                    USART1_PutUInt(seq);
                    USART1_PutString(", 回NAK\r\n");
                    OTA_SendResp(OTA_NAK);
                    continue;
                }

                /* CRC 校验通过, 写入 Flash */
                if (!headerDone && totalWrote == 0)
                {
                    /* 首包: 前 12 字节为固件头 */
                    memcpy(fwHeader, pktBuf, sizeof(FwHeader_t));
                    FwHeader_t *hdr = (FwHeader_t *)fwHeader;

                    if (hdr->magic != OTA_FW_MAGIC || hdr->length == 0 ||
                        hdr->length > APP_MAX_SIZE)
                    {
                        USART1_PutString("[BOOT] 固件头无效 magic=0x");
                        USART1_PutHex(hdr->magic);
                        USART1_PutString(" len=");
                        USART1_PutUInt(hdr->length);
                        USART1_PutString(", 拒绝升级\r\n");
                        OTA_SendResp(OTA_CAN);
                        otaFailed = 1;
                        break;
                    }

                    fwLen = hdr->length;
                    USART1_PutString("[BOOT] 固件信息: ver=");   /* ⭐ PutHex自带0x前缀, 不再重复 */
                    USART1_PutHex(hdr->version);
                    USART1_PutString(" len=");
                    USART1_PutUInt(fwLen);
                    USART1_PutString(" crc32=");                  /* ⭐ PutHex自带0x前缀, 不再重复 */
                    USART1_PutHex(hdr->crc32);
                    USART1_PutString("\r\n");

                    /* ⭐ 两阶段提交执行点: 固件头验证通过, 此刻才擦除 APP 区.
                     * 此前旧 APP 完好, 任何失败(网关放弃/超时/CAN)均可瞬时回退.
                     * ⚠ 无条件重擦 (擦除幂等, 无副作用): 断电续知(PARTIAL)表明
                     * APP 区可能残留上次中断的数据, 必须写入前清干净, 否则
                     * 写 0 到非0xFF 地址触发 PGERR → "无法擦写"连续失败 (历史根因).
                     * 不能靠 appErased 猜测"已擦过"跳过 —— 擦没擦只在本次会话有效,
                     * 断电重启后局部变量丢失, 一切以"先擦再写"为准 */
                    OTA_EraseAppArea();
                    Flash_SetAppPartial(1); /* ⭐ 持久化: 断电重启仍能得知APP区不完整 */
                    appErased = 1;
                    startTick = Boot_GetTick(); /* 擦除后重新计时总超时 */
                    USART1_PutString("[BOOT] 固件头有效, APP区已擦除, 开始写入...\r\n");

                    /* 写入固件头后的数据(跳过头) */
                    if (OTA_FlashWrite(APP_ADDR, pktBuf + sizeof(FwHeader_t),
                                       OTA_PACKET_DATA_SIZE - sizeof(FwHeader_t)))
                    {
                        totalWrote = OTA_PACKET_DATA_SIZE - sizeof(FwHeader_t);
                    }
                    else
                    {
                        USART1_PutString("[BOOT] Flash写入失败 @");   /* ⭐ PutHex自带0x前缀 */
                        USART1_PutHex(APP_ADDR);
                        USART1_PutString("\r\n");
                        OTA_SendResp(OTA_NAK);
                        continue;
                    }
                    headerDone = 1;
                }
                else
                {
                    /* 后续包: 直接写入 */
                    if (OTA_FlashWrite(APP_ADDR + totalWrote, pktBuf, OTA_PACKET_DATA_SIZE))
                    {
                        totalWrote += OTA_PACKET_DATA_SIZE;
                    }
                    else
                    {
                        USART1_PutString("[BOOT] Flash写入失败 @");   /* ⭐ PutHex自带0x前缀 */
                        USART1_PutHex(APP_ADDR + totalWrote);
                        USART1_PutString("\r\n");
                        OTA_SendResp(OTA_NAK);
                        continue;
                    }
                }

                /* 进度显示: 首包 + 每10包 */
                if (pktCount == 1 || (pktCount % 10) == 0)
                {
                    USART1_PutString("[BOOT] 接收包 #");
                    USART1_PutUInt(pktCount);
                    USART1_PutString(", 已写入 ");
                    USART1_PutUInt(totalWrote);
                    USART1_PutString("B\r\n");
                }

                /* ⭐ S18: 写盘成功, 推进期待序号 */
                otaExpectSeq++;

                /* 回复 ACK */
                OTA_SendResp(OTA_ACK);
            }
            else
            {
                /* CRC 错误, 回复 NAK 请求重发 */
                USART1_PutString("[BOOT] 包 #");
                USART1_PutUInt(pktCount);
                USART1_PutString(" CRC16校验失败 (recv=0x");
                USART1_PutHex(crcRecv);
                USART1_PutString(" calc=0x");
                USART1_PutHex(crcCalc);
                USART1_PutString(")\r\n");
                OTA_SendResp(OTA_NAK);
            }
        }
        else if (ret == 2)
        {
            /* 收到 EOT, 传输结束 */
            USART1_PutString("[BOOT] 收到EOT, 共 ");
            USART1_PutUInt(pktCount);
            USART1_PutString(" 包 ");
            USART1_PutUInt(totalWrote);
            USART1_PutString("B, CRC32校验中...\r\n");
            break;
        }
        else if (ret == 3)
        {
            /* 收到 CAN, 取消传输 */
            USART1_PutString("[BOOT] 收到CAN, 取消升级\r\n");
            otaFailed = 1;
            break;
        }
        /* ret == 0: 超时, 继续等待 */

        Boot_FeedWatchdog();
    }

    if (otaFailed)
    {
        USART1_PutString("[BOOT] 升级失败\r\n");
        OTA_AbortRecovery(appErased);       /* ⭐ P0-B: 统一清标志+尽力回退 */
    }
    else
    {
        /* 校验固件 CRC32 */
        FwHeader_t *hdr = (FwHeader_t *)fwHeader;
        uint32_t calcCrc32 = CRC32_Soft((const uint8_t *)APP_ADDR, fwLen);

        if (calcCrc32 == hdr->crc32)
        {
            USART1_PutString("[BOOT] CRC32校验通过! 启动APP\r\n");
            Flash_SaveOtaFlagKeepPartial(OTA_FLAG_DONE);
            Flash_SetAppPartial(0);     /* ⭐ 升级成功, 标记 APP 区完整 */
            OTA_SendResp(OTA_ACK);

            Boot_Delay(100);

            Load_APP(APP_ADDR);
        }
        else
        {
            USART1_PutString("[BOOT] CRC32校验失败! calc=0x");
            USART1_PutHex(calcCrc32);
            USART1_PutString(" expect=0x");
            USART1_PutHex(hdr->crc32);
            USART1_PutString("\r\n");
            /* ⭐ P0-B: 校验失败同样清标志防死循环 + 尽力回退 */
            OTA_AbortRecovery(appErased);
        }
    }
}

/* ==================== USART1 调试串口(PA9-TX, 115200) ==================== */

static void USART1_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1 | RCC_APB2Periph_AFIO, ENABLE);

    /* PA9 TX */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_9;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* USART1 配置 */
    USART_StructInit(&usart);
    usart.USART_BaudRate = 115200;
    usart.USART_Mode = USART_Mode_Tx;
    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);
}

static void USART1_PutString(const char *s)
{
    while (*s)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        USART_SendData(USART1, *s++);
    }
}

static void USART1_PutChar(char c)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    USART_SendData(USART1, c);
}

static void USART1_PutUInt(uint32_t val)
{
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (val == 0) { USART1_PutChar('0'); return; }
    while (val > 0 && i > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    USART1_PutString(&buf[i]);
}

static void USART1_PutHex(uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    USART1_PutChar('0'); USART1_PutChar('x');
    int started = 0;
    for (int i = 28; i >= 0; i -= 4)
    {
        uint8_t nib = (val >> i) & 0xF;
        if (nib || started || i == 0) { USART1_PutChar(hex[nib]); started = 1; }
    }
}

/* ==================== 初始化 ==================== */

void Boot_Init(void)
{
    /* 硬件初始化(SystemInit 已在启动文件中完成) */
    USART1_Init();                          /* 调试串口 (USART1, 115200) */
    USART1_PutString("\r\n[BOOT] BootLoader v1.0 启动\r\n");
    
    /* 等待系统稳定 */
    for (volatile int i = 0; i < 10000; i++);
    
    SysTick_Init();
    USART1_PutString("[BOOT] SysTick OK\r\n");
    
    USART2_Init(LORA_BAUD);
    USART1_PutString("[BOOT] USART2 OK\r\n");
    
    IWDG_Init(2500);                        /* 4s 超时 */
    USART1_PutString("[BOOT] IWDG OK\r\n");
    
    Key_Init();
    USART1_PutString("[BOOT] Key OK\r\n");
    USART1_PutString("[BOOT] 硬件初始化完成\r\n");
}

/* ==================== 主程序 ==================== */

int main(void)
{
    Boot_Init();

    /* 首次启动将节点身份固化到配置区(之后永不重写, OTA 不影响) */
    NodeConfig_Init();

    /* 检查升级标志 */
    uint32_t flag = Flash_ReadOtaFlag();

    if (flag == OTA_FLAG_GO)
    {
        /* 强制升级: 直接进入 Boot 模式 */
        USART1_PutString("[BOOT] OTA升级标志, 进入升级模式\r\n");
        OTA_Process();   /* 成功→跳APP不返回; 失败→内部清标志并尽力回退旧APP */

        /* ⭐ P0-D: 升级未完成且旧 APP 无法启动(已被覆盖)时,
         * 保持 Boot 可救状态: 持续监听按键/AT+OTA 命令, 随时可重新升级,
         * 不再是无响应死循环. 注意: OTA_AbortRecovery 内部已清 GO 标志,
         * 此处需重新立标志再复位, 才能再次进入升级模式 */
        USART1_PutString("[BOOT] 等待重新升级 (按KEY 或 LoRa AT+OTA)\r\n");
        while (1)
        {
            Boot_FeedWatchdog();

            if (Key_IsPressed())
            {
                Boot_Delay(50);             /* 去抖 */
                if (!Key_IsPressed())
                    continue;
                USART1_PutString("[BOOT] 按键触发, 重新升级\r\n");
                Flash_SaveOtaFlagKeepPartial(OTA_FLAG_GO);
                Boot_Delay(100);            /* 等串口发完再复位 */
                NVIC_SystemReset();
            }

            if (OTA_CollectAtCmd())
                OTA_HandleTriggerCmd();     /* 回ACK + 写GO + 复位 */
        }
    }
    else
    {
        /* 正常启动: 等待按键或 LoRa OTA 命令 */
        uint32_t enterTick = Boot_GetTick();
        uint8_t  enterBoot = 0;

        while ((Boot_GetTick() - enterTick) < BOOT_ENTER_WINDOW_MS)
        {
            if (Key_IsPressed())
            {
                enterBoot = 1;
                break;
            }

            /* 检查 LoRa OTA 命令: 完整 AT+OTA 指令(替代原裸字符 'O') */
            if (OTA_CollectAtCmd())
            {
                OTA_HandleTriggerCmd();     /* 回ACK + 写GO + 复位, 不返回 */
                enterBoot = 1;
                break;
            }

            Boot_FeedWatchdog();
        }

        if (enterBoot)
        {
            /* 进入 Boot 模式 */
            USART1_PutString("[BOOT] 按键/LoRa触发, 进入升级模式\r\n");
            OTA_Process();
        }
        else
        {
            /* 跳转 APP */
            USART1_PutString("[BOOT] 无升级请求, 跳转 APP...\r\n");
            Boot_Delay(50);                 /* 等待串口发送完成 */
            Load_APP(APP_ADDR);
        }
    }

    /* ⭐ P0-D 修复: 升级失败或 APP 跳转失败(栈顶校验不通过)时,
     * 不再是无响应 while(1) 盲区——持续监听按键/AT+OTA 可救活,
     * 随时重新进入升级模式 */
    while (1)
    {
        Boot_FeedWatchdog();

        if (Key_IsPressed())
        {
            Boot_Delay(50);                 /* 去抖 */
            if (!Key_IsPressed())
                continue;
            USART1_PutString("[BOOT] 按键触发, 重新升级\r\n");
            Flash_SaveOtaFlagKeepPartial(OTA_FLAG_GO);
            Boot_Delay(100);                /* 等串口发完再复位 */
            NVIC_SystemReset();
        }

        if (OTA_CollectAtCmd())
            OTA_HandleTriggerCmd();         /* 回ACK + 写GO + 复位 */
    }
}
