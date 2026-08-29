/**
 * @file bsp_qmc5883p.c
 * @brief QMC5883P 三轴磁力计驱动文件 (C语言版本)
 * 
 * 通过软件 I2C (GPIO 模拟) 读取传感器数据
 * 引脚映射:
 *   - SCL  = PB13
 *   - SDA  = PB14
 *   - DRDY = PB12 (数据就绪引脚)
 * 
 * @author Adapted from WilliTourt's C++ version
 * @version 1.0
 * @date 2026.07.19
 */

#include "bsp_qmc5883p.h"
#include "bsp_delay.h"
#include "bsp_usart.h"

/* ========================================================================
 * 软件 I2C 总线驱动
 * ======================================================================== */

/* ---------- GPIO 引脚操作宏 ---------- */

#define QMC_SCL_LOW()       GPIO_ResetBits(QMC_SCL_PORT, QMC_SCL_PIN)
#define QMC_SCL_HIGH()      GPIO_SetBits(QMC_SCL_PORT, QMC_SCL_PIN)
#define QMC_SDA_LOW()       GPIO_ResetBits(QMC_SDA_PORT, QMC_SDA_PIN)
#define QMC_SDA_HIGH()      GPIO_SetBits(QMC_SDA_PORT, QMC_SDA_PIN)
#define QMC_SDA_READ()      GPIO_ReadInputDataBit(QMC_SDA_PORT, QMC_SDA_PIN)

/* I2C 时序延迟 (约5us, 目标频率 ~100kHz) */
#define QMC_IIC_DELAY()     DelayXus(5)

/**
 * @brief 初始化软件 I2C 的 GPIO 引脚
 * 
 * SCL 和 SDA 配置为开漏输出 (OD), DRDY 配置为浮空输入
 * 逻辑 1 = 高电平 (由外部上拉提供), 逻辑 0 = 低电平
 */
static void QMC_IIC_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    /* 使能 GPIOB 时钟 */
    RCC_APB2PeriphClockCmd(QMC_SCL_RCC | QMC_SDA_RCC | QMC_DRDY_RCC, ENABLE);

    /* SCL - 时钟输出 */
    gpio.GPIO_Pin   = QMC_SCL_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(QMC_SCL_PORT, &gpio);

    /* SDA - 数据输入输出 */
    gpio.GPIO_Pin   = QMC_SDA_PIN;
    GPIO_Init(QMC_SDA_PORT, &gpio);

    /* DRDY - 数据就绪输入 */
    gpio.GPIO_Pin   = QMC_DRDY_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(QMC_DRDY_PORT, &gpio);

    /* 初始状态 (总线空闲) */
    QMC_SCL_HIGH();
    QMC_SDA_HIGH();
}

/**
 * @brief I2C 起始信号
 * 
 * SCL 高电平时 SDA 由高变低
 */
static void QMC_IIC_Start(void)
{
    QMC_SDA_HIGH();
    QMC_IIC_DELAY();
    QMC_SCL_HIGH();
    QMC_IIC_DELAY();
    QMC_SDA_LOW();      /* SDA 拉低 = 起始条件 */
    QMC_IIC_DELAY();
    QMC_SCL_LOW();      /* 拉低 SCL 准备传输数据 */
    QMC_IIC_DELAY();
}

/**
 * @brief I2C 停止信号
 * 
 * SCL 高电平时 SDA 由低变高
 */
static void QMC_IIC_Stop(void)
{
    QMC_SDA_LOW();
    QMC_IIC_DELAY();
    QMC_SCL_HIGH();
    QMC_IIC_DELAY();
    QMC_SDA_HIGH();     /* SDA 上升沿 = 停止条件 */
    QMC_IIC_DELAY();
}

/**
 * @brief 等待应答信号
 * 
 * 主机释放 SDA, 从机在第 9 个时钟周期将 SDA 拉低
 * 
 * @return 0 = 收到应答 (ACK), 1 = 未收到应答 (NACK)
 */
static uint8_t QMC_IIC_WaitAck(void)
{
    uint8_t ack = 0;
    uint16_t timeout = 0;

    QMC_SDA_HIGH();     /* 释放 SDA, 由从机控制 */
    QMC_IIC_DELAY();
    QMC_SCL_HIGH();     /* 第 9 个时钟脉冲 */
    QMC_IIC_DELAY();

    /* 等待 SDA 被从机拉低 (ACK) */
    while (QMC_SDA_READ())
    {
        timeout++;
        if (timeout > 500)
        {
            ack = 1;    /* 超时 = NACK */
            break;
        }
    }

    QMC_SCL_LOW();
    QMC_IIC_DELAY();
    return ack;
}

/**
 * @brief 发送一个字节 (MSB 先行)
 * 
 * @param data 待发送的字节数据
 */
static void QMC_IIC_SendByte(uint8_t data)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        if (data & 0x80)
            QMC_SDA_HIGH();
        else
            QMC_SDA_LOW();

        data <<= 1;
        QMC_IIC_DELAY();
        QMC_SCL_HIGH();
        QMC_IIC_DELAY();
        QMC_SCL_LOW();  /* 拉低 SCL 准备下一位数据 */
        QMC_IIC_DELAY();
    }
}

/**
 * @brief 接收一个字节 (MSB 先行)
 * 
 * @param ack 应答标志 (0 = 发送 ACK, 1 = 发送 NACK)
 * @return 接收到的字节数据
 */
static uint8_t QMC_IIC_RecvByte(uint8_t ack)
{
    uint8_t data = 0;

    QMC_SDA_HIGH();     /* 释放 SDA, 由从机控制 */

    for (uint8_t i = 0; i < 8; i++)
    {
        data <<= 1;
        QMC_SCL_HIGH();
        QMC_IIC_DELAY();
        if (QMC_SDA_READ())
            data |= 0x01;
        QMC_SCL_LOW();
        QMC_IIC_DELAY();
    }

    /* 发送应答位 */
    if (ack)
        QMC_SDA_HIGH(); /* NACK: 拉高 SDA 表示结束 */
    else
        QMC_SDA_LOW();  /* ACK: 拉低 SDA 表示继续 */

    QMC_IIC_DELAY();
    QMC_SCL_HIGH();
    QMC_IIC_DELAY();
    QMC_SCL_LOW();
    QMC_IIC_DELAY();
    QMC_SDA_HIGH();     /* 释放 SDA */

    return data;
}

/* ========================================================================
 * I2C 通信函数 (寄存器级)
 * ======================================================================== */

/**
 * @brief 向 I2C 设备写入寄存器
 * 
 * 时序: START + 从机地址(W) + 寄存器地址 + 数据写入 + STOP
 * 
 * @param reg  寄存器地址
 * @param data 待写入数据缓冲区
 * @param len  数据长度 (字节)
 * @return 0 = 成功, 1 = 失败
 */
static uint8_t QMC_I2C_WriteReg(uint8_t reg, uint8_t *data, uint8_t len)
{
    QMC_IIC_Start();

    /* 发送从机地址 + 写标志 */
    QMC_IIC_SendByte(QMC5883P_ADDR << 1);
    if (QMC_IIC_WaitAck()) goto _err;

    /* 发送寄存器地址 */
    QMC_IIC_SendByte(reg);
    if (QMC_IIC_WaitAck()) goto _err;

    /* 发送数据 */
    for (uint8_t i = 0; i < len; i++)
    {
        QMC_IIC_SendByte(data[i]);
        if (QMC_IIC_WaitAck()) goto _err;
    }

    QMC_IIC_Stop();
    return 0;

_err:
    QMC_IIC_Stop();
    return 1;
}

/**
 * @brief 从 I2C 设备读取寄存器
 * 
 * 时序: START + 从机地址(W) + 寄存器地址 + RESTART + 从机地址(R) + 数据读取 + STOP
 * 
 * @param reg  寄存器地址
 * @param data 数据接收缓冲区
 * @param len  读取长度
 * @return 0 = 成功, 1 = 失败
 */
static uint8_t QMC_I2C_ReadReg(uint8_t reg, uint8_t *data, uint8_t len)
{
    QMC_IIC_Start();

    /* 发送从机地址 + 写标志 (先写寄存器地址) */
    QMC_IIC_SendByte(QMC5883P_ADDR << 1);
    if (QMC_IIC_WaitAck()) goto _err;

    /* 发送寄存器地址 */
    QMC_IIC_SendByte(reg);
    if (QMC_IIC_WaitAck()) goto _err;

    /* 重启总线 */
    QMC_IIC_Start();

    /* 发送从机地址 + 读标志 */
    QMC_IIC_SendByte((QMC5883P_ADDR << 1) | 0x01);
    if (QMC_IIC_WaitAck()) goto _err;

    /* 连续读取 len-1 个字节发 ACK, 最后一字节发 NACK */
    for (uint8_t i = 0; i < len; i++)
    {
        data[i] = QMC_IIC_RecvByte(i == len - 1);
    }

    QMC_IIC_Stop();
    return 0;

_err:
    QMC_IIC_Stop();
    return 1;
}

/* ========================================================================
 * QMC5883P 内部功能
 * ======================================================================== */

/**
 * @brief 检查数据是否就绪
 * 
 * 读取 DRDY 引脚电平状态
 * QMC5883P 在新数据准备好后会将 DRDY 拉高
 * 读取数据后自动清零
 * 
 * @return 0 = 数据未就绪, 1 = 数据就绪
 */
static uint8_t QMC5883P_IsDataRdy(void)
{
    return GPIO_ReadInputDataBit(QMC_DRDY_PORT, QMC_DRDY_PIN);
}

/* ========================================================================
 * QMC5883P 延迟函数
 * ======================================================================== */

/**
 * @brief 毫秒级延迟 (阻塞)
 * 
 * @param ms 延迟时长 (毫秒)
 */
static void QMC5883P_Delay(uint32_t ms)
{
    DelayXms(ms);
}

/**
 * @brief 获取系统滴答计数 (毫秒)
 */
static uint32_t QMC5883P_GetTick(void)
{
    return Get_Tick();
}

void QMC5883P_Init(QMC5883P_Device_t *dev, QMC5883P_Mode_t mode,
                   QMC5883P_Spd_t speed, QMC5883P_Rng_t range)
{
    uint8_t ret;

    /* 保存配置参数 */
    dev->mode  = (uint8_t)mode;
    dev->speed = (uint8_t)speed;
    dev->range = (uint8_t)range;

    /* 根据量程设置灵敏度 */
    switch (dev->range)
    {
        case QMC5883P_RNG_2G:  dev->sensitivity = QMC5883P_SENS_2G;  break;
        case QMC5883P_RNG_8G:  dev->sensitivity = QMC5883P_SENS_8G;  break;
        case QMC5883P_RNG_12G: dev->sensitivity = QMC5883P_SENS_12G; break;
        case QMC5883P_RNG_30G: dev->sensitivity = QMC5883P_SENS_30G; break;
        default:               dev->sensitivity = QMC5883P_SENS_2G;  break;
    }

    /* 设置硬编码校准值 */
    dev->offset_x = MAG_X_OFFSET;
    dev->offset_y = MAG_Y_OFFSET;
    dev->offset_z = MAG_Z_OFFSET;
    dev->scale_x  = MAG_X_SCALE;
    dev->scale_y  = MAG_Y_SCALE;
    dev->scale_z  = MAG_Z_SCALE;

    /* 初始化磁场数据 */
    dev->mag_x = 0.0f;
    dev->mag_y = 0.0f;
    dev->mag_z = 0.0f;

    /* 初始化软件 I2C 引脚 */
    QMC_IIC_GPIO_Init();

    /* 硬件初始化 (Begin) */
    ret = QMC5883P_Begin(dev);
    if (ret == QMC5883P_OK)
        Usart_Printf(USART_DEBUG, "QMC5883P Init OK!\n");
    else
        Usart_Printf(USART_DEBUG, "QMC5883P Init FAIL! ret=%d\n", ret);
}

uint8_t QMC5883P_Begin(QMC5883P_Device_t *dev)
{
    uint8_t data;

    QMC5883P_Delay(20);

    /* ---- 第 1 步: 软复位 ---- */
    data = QMC5883P_CTRL2_SOFT_RESET;
    if (QMC_I2C_WriteReg(QMC5883P_REG_CONTROL_2, &data, 1))
        return QMC5883P_ERROR;

    QMC5883P_Delay(20);

    /* 退出复位模式 */
    data = 0x00;
    if (QMC_I2C_WriteReg(QMC5883P_REG_CONTROL_2, &data, 1))
        return QMC5883P_ERROR;

    QMC5883P_Delay(20);

    /* ---- 第 2 步: 验证芯片 ID ---- */
    if (QMC_I2C_ReadReg(QMC5883P_REG_CHIP_ID, &data, 1))
        return QMC5883P_ERROR;

    if (data != QMC5883P_CHIP_ID)
        return QMC5883P_ERROR_ID;

    /* ---- 第 3 步: 设置量程 (Control 2) ---- */
    data = dev->range;
    if (QMC_I2C_WriteReg(QMC5883P_REG_CONTROL_2, &data, 1))
        return QMC5883P_ERROR;

    /* ---- 第 4 步: 设置运行参数 (Control 1) ---- */
    /* OSR 预留 2 (总过采样率 8x, 实际采样 2x) */
    data = dev->mode | dev->speed |
           QMC5883P_CTRL1_OSR1_2 | QMC5883P_CTRL1_OSR2_2;
    if (QMC_I2C_WriteReg(QMC5883P_REG_CONTROL_1, &data, 1))
        return QMC5883P_ERROR;

    return QMC5883P_OK;
}

uint8_t QMC5883P_Update(QMC5883P_Device_t *dev)
{
    uint8_t rawData[6];
    uint32_t start = QMC5883P_GetTick();

    /* 等待数据就绪 (或超时返回)
     * ⭐ 双重超时兜底: Get_Tick()(ms级) + 软件自增计数(次), 防止SysTick停了死循环 */
    uint32_t swTimeout = 0;  /* 软件超时计数(次), 每次DelayXms(1) = 1次, 最大QMC_READ_TIMEOUT次 */
    #define QMC_SW_TIMEOUT_CNT QMC5883P_READ_TIMEOUT_MS
    while (!QMC5883P_IsDataRdy())
    {
        swTimeout++;
        if ((QMC5883P_GetTick() - start > QMC5883P_READ_TIMEOUT_MS) || (swTimeout >= QMC_SW_TIMEOUT_CNT))
            return QMC5883P_ERROR;
        QMC5883P_Delay(1);
    }

    /* 连续读取 6 个字节 (X, Y, Z 各 2 字节) */
    if (QMC_I2C_ReadReg(QMC5883P_REG_XOUT_L, rawData, 6))
        return QMC5883P_ERROR;

    /* 合成 16 位原始值 (注意: 小端格式) */
    int16_t raw_x = (int16_t)(rawData[1] << 8 | rawData[0]);
    int16_t raw_y = (int16_t)(rawData[3] << 8 | rawData[2]);
    int16_t raw_z = (int16_t)(rawData[5] << 8 | rawData[4]);

    /* 磁力计算: (原始值/灵敏度 - 偏移) * 缩放 */
    dev->mag_x = ((float)raw_x / (float)dev->sensitivity - dev->offset_x) * dev->scale_x;
    dev->mag_y = ((float)raw_y / (float)dev->sensitivity - dev->offset_y) * dev->scale_y;
    dev->mag_z = ((float)raw_z / (float)dev->sensitivity - dev->offset_z) * dev->scale_z;

    return QMC5883P_OK;
}

/****************************************************************************
 * 函数名: QMC5883P_Calibration
 * 功能:   校准 QMC5883P 设备: 采集30秒各轴最大最小值, 计算并打印校准参数
 * 参数:   dev - 设备结构体指针
 * 返回值: 无
 * 说明:   1. 函数内部自动清零补偿参数, 调用前无需修改宏
 *         2. 校准期间缓慢旋转设备, 让传感器朝向各个方向
 *         3. 结束后串口打印6行 #define, 复制到 bsp_qmc5883p.h 即可
 *         4. 本函数为阻塞式校准, 校准完成后删除调用
 ****************************************************************************/
void QMC5883P_Calibration(QMC5883P_Device_t *dev)
{
    /* 采集前先清零补偿参数, 确保读到的是原始磁场值 (无需手动改宏) */
    dev->offset_x = 0.0f;
    dev->offset_y = 0.0f;
    dev->offset_z = 0.0f;
    dev->scale_x  = 1.0f;
    dev->scale_y  = 1.0f;
    dev->scale_z  = 1.0f;

    float max[3] = {-9999.0f, -9999.0f, -9999.0f};
    float min[3] = { 9999.0f,  9999.0f,  9999.0f};
    float cur[3];
    uint32_t startTick = QMC5883P_GetTick();
    uint32_t printTick = startTick;

    Usart_Printf(USART_DEBUG, "\r\n=== QMC5883P Calibration Start (30s) ===\r\n");
    Usart_Printf(USART_DEBUG, "Rotate device slowly in all directions...\r\n");

    while (QMC5883P_GetTick() - startTick < 30000)
    {
        if (QMC5883P_Update(dev) == QMC5883P_OK)
        {
            cur[0] = dev->mag_x;
            cur[1] = dev->mag_y;
            cur[2] = dev->mag_z;

            for (int i = 0; i < 3; i++)
            {
                if (cur[i] > max[i]) max[i] = cur[i];
                if (cur[i] < min[i]) min[i] = cur[i];
            }
        }

        /* 每秒打印一次采集进度 */
        if (QMC5883P_GetTick() - printTick >= 1000)
        {
            Usart_Printf(USART_DEBUG, "[%3ds] X:%.2f~%.2f  Y:%.2f~%.2f  Z:%.2f~%.2f\r\n",
                (QMC5883P_GetTick() - startTick) / 1000,
                min[0], max[0], min[1], max[1], min[2], max[2]);
            printTick = QMC5883P_GetTick();
        }
    }

    /* 计算偏移(硬铁)与缩放(软铁) */
    float offset[3], scale[3], delta[3], avg;
    for (int i = 0; i < 3; i++)
        delta[i] = (max[i] - min[i]) / 2.0f;
    avg = (delta[0] + delta[1] + delta[2]) / 3.0f;

    for (int i = 0; i < 3; i++)
    {
        offset[i] = (max[i] + min[i]) / 2.0f;
        scale[i]  = (delta[i] != 0.0f) ? (avg / delta[i]) : 1.0f;
    }

    /* 打印最终结果 */
    Usart_Printf(USART_DEBUG, "\r\n=== Calibration Done ===\r\n");
    Usart_Printf(USART_DEBUG, "Copy these 6 lines to bsp_qmc5883p.h:\r\n\r\n");
    Usart_Printf(USART_DEBUG, "#define MAG_X_OFFSET    %.6ff\r\n", offset[0]);
    Usart_Printf(USART_DEBUG, "#define MAG_Y_OFFSET    %.6ff\r\n", offset[1]);
    Usart_Printf(USART_DEBUG, "#define MAG_Z_OFFSET    %.6ff\r\n", offset[2]);
    Usart_Printf(USART_DEBUG, "#define MAG_X_SCALE     %.6ff\r\n", scale[0]);
    Usart_Printf(USART_DEBUG, "#define MAG_Y_SCALE     %.6ff\r\n", scale[1]);
    Usart_Printf(USART_DEBUG, "#define MAG_Z_SCALE     %.6ff\r\n", scale[2]);
    Usart_Printf(USART_DEBUG, "\r\nCalibration finished. Remove QMC5883P_Calibration() call.\r\n");

    while (1);
}
