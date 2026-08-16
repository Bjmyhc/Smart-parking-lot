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
#include "boot.h"
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
static uint8_t OTA_ReceivePacket(uint8_t *data, uint16_t *seq, uint16_t *crc);
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

/* ==================== Flash 操作 ==================== */

uint32_t OTA_ReadFlag(void)
{
    return *(volatile uint32_t *)OTA_FLAG_ADDR;
}

void OTA_WriteFlag(uint32_t flag)
{
    FLASH_Unlock();
    /* 擦除标志页 */
    FLASH_ErasePage(OTA_FLAG_ADDR);
    /* 写入标志 */
    FLASH_ProgramHalfWord(OTA_FLAG_ADDR, (uint16_t)(flag & 0xFFFF));
    FLASH_ProgramHalfWord(OTA_FLAG_ADDR + 2, (uint16_t)((flag >> 16) & 0xFFFF));
    FLASH_Lock();
}

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

        if (FLASH_ProgramHalfWord(addr + i, halfWord) == FLASH_COMPLETE)
        {
            /* 读回验证 */
            if (*(volatile uint16_t *)(addr + i) != halfWord)
            {
                FLASH_Lock();
                return 0;
            }
        }
        else
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

/* ==================== Xmodem 包接收 ==================== */

static uint8_t OTA_ReceivePacket(uint8_t *data, uint16_t *seq, uint16_t *crc)
{
    RxState_t state = RX_STATE_WAIT_SOH;
    uint8_t b;
    uint16_t idx = 0;
    uint32_t timeout = Boot_GetTick();
    uint8_t bufSeqH = 0, bufSeqL = 0;
    uint8_t bufCrcH = 0, bufCrcL = 0;

    while ((Boot_GetTick() - timeout) < OTA_PACKET_TIMEOUT_MS)
    {
        if (USART2_ReadByte(&b))
        {
            timeout = Boot_GetTick();       /* 收到数据, 重置超时 */

            switch (state)
            {
                case RX_STATE_WAIT_SOH:
                    if (b == SOH)
                    {
                        state = RX_STATE_WAIT_SEQ_H;
                        idx = 0;
                    }
                    else if (b == EOT)
                    {
                        /* 传输结束 */
                        return 2;
                    }
                    else if (b == CAN)
                    {
                        /* 取消传输 */
                        return 3;
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

    USART1_PutString("[BOOT] 开始OTA升级, 准备接收APP固件...\r\n");

    /* 擦除 APP 分区 */
    OTA_EraseAppArea();

    USART1_PutString("[BOOT] APP分区已擦除, 开始接收数据...\r\n");

    /* 接收固件数据 */
    while (1)
    {
        /* 超时检查 */
        if ((Boot_GetTick() - startTick) >= OTA_TOTAL_TIMEOUT_MS)
        {
            USART1_PutString("[BOOT] 超时, 已接收 ");
            USART1_PutUInt(pktCount);
            USART1_PutString(" 包 ");
            USART1_PutUInt(totalWrote);
            USART1_PutString("B\r\n");
            otaFailed = 1;
            break;
        }

        /* 接收数据包 */
        ret = OTA_ReceivePacket(pktBuf, &seq, &crcRecv);

        if (ret == 1)
        {
            pktCount++;
            /* 数据包接收成功, 验证 CRC16 */
            crcCalc = CRC16_XMODEM(pktBuf, OTA_PACKET_DATA_SIZE);

            if (crcCalc == crcRecv)
            {
                /* CRC 校验通过, 写入 Flash */
                if (!headerDone && totalWrote == 0)
                {
                    /* 首包: 前 12 字节为固件头 */
                    memcpy(fwHeader, pktBuf, sizeof(FwHeader_t));
                    FwHeader_t *hdr = (FwHeader_t *)fwHeader;

                    if (hdr->magic != FW_MAGIC)
                    {
                        USART1_PutString("[BOOT] 固件头错误 magic=0x");
                        USART1_PutHex(hdr->magic);
                        USART1_PutString(", 拒绝升级\r\n");
                        OTA_SendResp(CAN);
                        otaFailed = 1;
                        break;
                    }

                    fwLen = hdr->length;
                    USART1_PutString("[BOOT] 固件信息: ver=0x");
                    USART1_PutHex(hdr->version);
                    USART1_PutString(" len=");
                    USART1_PutUInt(fwLen);
                    USART1_PutString(" crc32=0x");
                    USART1_PutHex(hdr->crc32);
                    USART1_PutString("\r\n");

                    /* 写入固件头后的数据(跳过头) */
                    if (OTA_FlashWrite(APP_ADDR, pktBuf + sizeof(FwHeader_t),
                                       OTA_PACKET_DATA_SIZE - sizeof(FwHeader_t)))
                    {
                        totalWrote = OTA_PACKET_DATA_SIZE - sizeof(FwHeader_t);
                    }
                    else
                    {
                        USART1_PutString("[BOOT] Flash写入失败 @0x");
                        USART1_PutHex(APP_ADDR);
                        USART1_PutString("\r\n");
                        OTA_SendResp(NAK);
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
                        USART1_PutString("[BOOT] Flash写入失败 @0x");
                        USART1_PutHex(APP_ADDR + totalWrote);
                        USART1_PutString("\r\n");
                        OTA_SendResp(NAK);
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

                /* 回复 ACK */
                OTA_SendResp(ACK);
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
                OTA_SendResp(NAK);
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
        OTA_SendResp(NAK);
    }
    else
    {
        /* 校验固件 CRC32 */
        FwHeader_t *hdr = (FwHeader_t *)fwHeader;
        uint32_t calcCrc32 = CRC32_Soft((const uint8_t *)APP_ADDR, fwLen);

        if (calcCrc32 == hdr->crc32)
        {
            USART1_PutString("[BOOT] CRC32校验通过! 启动APP\r\n");
            OTA_WriteFlag(OTA_FLAG_DONE);
            OTA_SendResp(ACK);

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
            OTA_SendResp(NAK);
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

    /* 检查升级标志 */
    uint32_t flag = OTA_ReadFlag();

    if (flag == OTA_FLAG_GO)
    {
        /* 强制升级: 直接进入 Boot 模式 */
        USART1_PutString("[BOOT] OTA升级标志, 进入升级模式\r\n");
        OTA_Process();
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

            /* 检查 LoRa OTA 命令 */
            uint8_t b;
            if (USART2_ReadByte(&b))
            {
                /* 如果收到字符 'O', 视为 OTA 启动命令 */
                if (b == 'O')
                {
                    enterBoot = 1;
                    break;
                }
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

    /* 如果升级失败或 APP 跳转失败, 停留在 Boot 并喂狗 */
    while (1)
    {
        Boot_FeedWatchdog();
    }
}
