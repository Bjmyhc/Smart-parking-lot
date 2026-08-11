/****************************************************************************
 * BootLoader 入口 - main.c
 *
 * 功能:
 *   上电启动分支: 检测升级标志/按键/OTA指令 → 进入 Boot 接收固件
 *   或跳转到 APP 区
 *
 * 固件接收协议:
 *   基于 Xmodem 思想, 链路改用 LoRa 定点传输(USART2)
 *   每包 128B + CRC16 + ACK/NAK 重发, 总超时 120s
 *
 * 硬件: STM32F103C8T6
 *   USART2(PA2-TX, PA3-RX) - LoRa 模块
 *   PA0  - FLASH 按键(低电平有效)
 *   IWDG - 独立看门狗, 4s 超时
 *
 * 参考: 0-BootLoader区例程（节点板程序）的 Load_APP 跳转
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
#include "stm32f10x_misc.h"

/* 函数指针类型(跳转用) */
typedef void (*pFunction)(void);

/* ==================== 局部变量 ==================== */
volatile uint32_t g_bootTick = 0;

/* 接收状态机 */
typedef enum {
    RX_STATE_WAIT_SOH,      /* 等待 SOH(0x02) */
    RX_STATE_WAIT_SEQ_H,    /* 等待包序号高字节 */
    RX_STATE_WAIT_SEQ_L,    /* 等待包序号低字节 */
    RX_STATE_WAIT_DATA,     /* 等待 128B 数据 */
    RX_STATE_WAIT_CRC_H,    /* 等待 CRC16 高字节 */
    RX_STATE_WAIT_CRC_L,    /* 等待 CRC16 低字节 */
    RX_STATE_DONE,          /* 一包完成 */
} RxState_t;

/* ==================== 本地函数声明 ==================== */
static void SysTick_Init(void);
static void USART2_Init(uint32_t baud);
static void IWDG_Init(uint16_t reload);
static void Key_Init(void);
static uint8_t Key_IsPressed(void);
static void USART2_SendByte(uint8_t b);
static uint8_t USART2_ReadByte(uint8_t *b);
static void Boot_ReceiveFirmware(void);
static uint8_t OTA_ReceivePacket(uint8_t *data, uint16_t *seq, uint16_t *crc);
static void OTA_Process(void);
static void Boot_Delay(uint32_t ms);

/* ==================== 系统滴答 ==================== */

void SysTick_Handler(void)
{
    g_bootTick++;
}

static void SysTick_Init(void)
{
    /* 使用系统时钟 72MHz, 1ms 中断 */
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

/* ==================== IWDG 看门狗 ==================== */

static void IWDG_Init(uint16_t reload)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);   /* LSI 40kHz / 64 ≈ 625Hz */
    IWDG_SetReload(reload);                 /* 2500 × 1.6ms = 4s */
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
        Boot_FeedWatchdog();                /* 延时期间喂狗, 防止复位 */
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
        Boot_FeedWatchdog();                /* 擦除慢, 中间喂狗 */
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

    /* 检查栈顶地址合法性 */
    stackTop = *(volatile uint32_t *)appxaddr;
    if ((stackTop & 0x2FFE0000) != 0x20000000)
        return;                             /* 非法, 留在 Boot */

    /* 关总中断 */
    __disable_irq();

    /* 关所有外设中断(可选, 但跳转前必须清 PendSV/SysTick) */
    SysTick->CTRL = 0;

    /* 设置 MSP 为 APP 的栈顶 */
    __set_MSP(stackTop);

    /* 取复位向量并跳转 */
    jump = (pFunction)(*(volatile uint32_t *)(appxaddr + 4));
    jump();
}

/* ==================== 接收一包数据 ==================== */

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
            timeout = Boot_GetTick();       /* 收到数据刷新超时 */

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
                        /* 传输结束标志 */
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
                    return 1;               /* 完整一包 */

                default:
                    break;
            }
        }

        Boot_FeedWatchdog();
    }

    return 0;                               /* 超时 */
}

/* ==================== OTA 接收主流程 ==================== */

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

    /* 擦除 APP 区 */
    OTA_EraseAppArea();

    /* 主接收循环 */
    while (1)
    {
        /* 总超时检查 */
        if ((Boot_GetTick() - startTick) >= OTA_TOTAL_TIMEOUT_MS)
        {
            otaFailed = 1;
            break;
        }

        /* 接收一包 */
        ret = OTA_ReceivePacket(pktBuf, &seq, &crcRecv);

        if (ret == 1)
        {
            /* 收到完整数据包, 校验 CRC16 */
            crcCalc = CRC16_XMODEM(pktBuf, OTA_PACKET_DATA_SIZE);

            if (crcCalc == crcRecv)
            {
                /* 数据正确, 写入 Flash */
                if (!headerDone && totalWrote == 0)
                {
                    /* 第一包: 前 12 字节是文件头 */
                    memcpy(fwHeader, pktBuf, sizeof(FwHeader_t));
                    FwHeader_t *hdr = (FwHeader_t *)fwHeader;

                    if (hdr->magic != FW_MAGIC)
                    {
                        /* 魔数不对, 不是合法固件, 发 CAN 取消 */
                        USART2_SendByte(CAN);
                        otaFailed = 1;
                        break;
                    }

                    fwLen = hdr->length;
                    /* 写入剩余数据(去掉文件头) */
                    if (OTA_FlashWrite(APP_ADDR, pktBuf + sizeof(FwHeader_t),
                                       OTA_PACKET_DATA_SIZE - sizeof(FwHeader_t)))
                    {
                        totalWrote = OTA_PACKET_DATA_SIZE - sizeof(FwHeader_t);
                    }
                    else
                    {
                        USART2_SendByte(NAK);
                        continue;
                    }
                    headerDone = 1;
                }
                else
                {
                    /* 后续包: 全部写入 */
                    if (OTA_FlashWrite(APP_ADDR + totalWrote, pktBuf, OTA_PACKET_DATA_SIZE))
                    {
                        totalWrote += OTA_PACKET_DATA_SIZE;
                    }
                    else
                    {
                        USART2_SendByte(NAK);
                        continue;
                    }
                }

                /* 发 ACK */
                USART2_SendByte(ACK);
            }
            else
            {
                /* CRC 错误, 发 NAK 要求重发 */
                USART2_SendByte(NAK);
            }
        }
        else if (ret == 2)
        {
            /* 收到 EOT, 传输结束 */
            break;
        }
        else if (ret == 3)
        {
            /* 收到 CAN, 取消传输 */
            otaFailed = 1;
            break;
        }
        /* ret == 0: 超时, 继续等 */

        Boot_FeedWatchdog();
    }

    if (otaFailed)
    {
        /* 失败: 保持 OTA 标志, 下次上电继续重试 */
        /* 发 NAK 通知网关失败 */
        USART2_SendByte(NAK);
    }
    else
    {
        /* 校验整个固件 CRC32 */
        FwHeader_t *hdr = (FwHeader_t *)fwHeader;
        uint32_t calcCrc32 = CRC32_Soft((const uint8_t *)APP_ADDR, fwLen);

        if (calcCrc32 == hdr->crc32)
        {
            /* 校验通过, 清标志, 跳转 APP */
            OTA_WriteFlag(OTA_FLAG_DONE);
            USART2_SendByte(ACK);           /* 通知网关成功 */

            Boot_Delay(100);                /* 等 ACK 发完 */

            /* 跳转 APP */
            Load_APP(APP_ADDR);
        }
        else
        {
            /* CRC32 不匹配, 失败 */
            USART2_SendByte(NAK);
        }
    }
}

/* ==================== 初始化 ==================== */

void Boot_Init(void)
{
    /* 时钟配置由启动文件(SystemInit)完成 */
    SysTick_Init();
    USART2_Init(LORA_BAUD);
    IWDG_Init(2500);                        /* 4s 超时 */
    Key_Init();
}

/* ==================== 主函数 ==================== */

int main(void)
{
    Boot_Init();

    /* 读取升级标志 */
    uint32_t flag = OTA_ReadFlag();

    if (flag == OTA_FLAG_GO)
    {
        /* 需要升级: 直接进入 Boot 接收模式 */
        OTA_Process();
    }
    else
    {
        /* 正常启动: 上电窗口等待按键或 OTA 指令 */
        uint32_t enterTick = Boot_GetTick();
        uint8_t  enterBoot = 0;

        while ((Boot_GetTick() - enterTick) < BOOT_ENTER_WINDOW_MS)
        {
            if (Key_IsPressed())
            {
                enterBoot = 1;
                break;
            }

            /* 检查是否有 LoRa OTA 指令 */
            uint8_t b;
            if (USART2_ReadByte(&b))
            {
                /* 简单识别: 收到 'O' 开头认为是 OTA 指令 */
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
            /* 进 Boot 接收模式 */
            OTA_Process();
        }
        else
        {
            /* 跳转 APP */
            Load_APP(APP_ADDR);
        }
    }

    /* 如果跳转失败或 OTA 完成, 停在这里等看门狗复位 */
    while (1)
    {
        Boot_FeedWatchdog();
    }
}