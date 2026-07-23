/**
 * @file bsp_qmc5883p.h
 * @brief QMC5883P 三轴磁力计驱动头文件 (C语言版本)
 * 
 * 通过软件模拟 I2C (GPIO 位翻转) 驱动 QMC5883P 磁力计
 * 提供配置、数据读取和校准功能，支持多种量程范围
 * 
 * 引脚映射:
 *   - SCL:  PB13 (软件 I2C 时钟线)
 *   - SDA:  PB14 (软件 I2C 数据线)
 *   - DRDY: PB12 (数据就绪中断引脚)
 * 
 * @author Adapted from WilliTourt's C++ version
 * @version 1.0
 * @date 2026.07.19
 */

#ifndef __BSP_QMC5883P_H
#define __BSP_QMC5883P_H

#include <stdint.h>
#include "stm32f10x.h"

/* ========================================================================
 * 引脚映射定义（硬件连接配置）
 * ======================================================================== */

/* I2C 时钟线 - PB14 */
#define QMC_SCL_PORT                    GPIOB
#define QMC_SCL_PIN                     GPIO_Pin_14
#define QMC_SCL_RCC                     RCC_APB2Periph_GPIOB

/* I2C 数据线 - PB13 */
#define QMC_SDA_PORT                    GPIOB
#define QMC_SDA_PIN                     GPIO_Pin_13
#define QMC_SDA_RCC                     RCC_APB2Periph_GPIOB

/* 数据就绪引脚 - PB12 */
#define QMC_DRDY_PORT                   GPIOB
#define QMC_DRDY_PIN                    GPIO_Pin_12
#define QMC_DRDY_RCC                    RCC_APB2Periph_GPIOB

/* ========================================================================
 * I2C 设备地址与参数
 * ======================================================================== */

#define QMC5883P_ADDR                   0x2C    /* I2C 设备地址 (7位) */
#define QMC5883P_CHIP_ID                0x80    /* 芯片 ID 值 */
#define QMC5883P_READ_TIMEOUT_MS        100     /* I2C 读取超时时间 (ms) */

/* ========================================================================
 * 寄存器地址定义
 * ======================================================================== */

#define QMC5883P_REG_CHIP_ID            0x00    /* 芯片 ID 寄存器 */
#define QMC5883P_REG_XOUT_L             0x01    /* X 轴数据低字节 */
#define QMC5883P_REG_XOUT_H             0x02    /* X 轴数据高字节 */
#define QMC5883P_REG_YOUT_L             0x03    /* Y 轴数据低字节 */
#define QMC5883P_REG_YOUT_H             0x04    /* Y 轴数据高字节 */
#define QMC5883P_REG_ZOUT_L             0x05    /* Z 轴数据低字节 */
#define QMC5883P_REG_ZOUT_H             0x06    /* Z 轴数据高字节 */
#define QMC5883P_REG_STATUS             0x09    /* 状态寄存器 (OVFL, DRDY) */
#define QMC5883P_REG_CONTROL_1          0x0A    /* 控制寄存器 1 (OSR, ODR, MODE) */
#define QMC5883P_REG_CONTROL_2          0x0B    /* 控制寄存器 2 (量程,自检,复位,软复位) */

/* ========================================================================
 * 状态寄存器位定义
 * ======================================================================== */

#define QMC5883P_STATUS_DRDY            0x01    /* 数据就绪标志 */
#define QMC5883P_STATUS_OVL             0x02    /* 溢出标志 */

/* ========================================================================
 * 控制寄存器 1 位定义
 * ======================================================================== */

/* 工作模式 */
#define QMC5883P_CTRL1_MODE_SUSPEND     0x00    /* 挂起模式 (待机) */
#define QMC5883P_CTRL1_MODE_NORMAL      0x01    /* 正常模式 */
#define QMC5883P_CTRL1_MODE_SINGLE      0x02    /* 单次测量模式 */
#define QMC5883P_CTRL1_MODE_CONT        0x03    /* 连续测量模式 */

/* 过采样率 (OSR1) */
#define QMC5883P_CTRL1_OSR1_8           0x00    /* 过采样率: 8 */
#define QMC5883P_CTRL1_OSR1_4           0x10    /* 过采样率: 4 */
#define QMC5883P_CTRL1_OSR1_2           0x20    /* 过采样率: 2 */
#define QMC5883P_CTRL1_OSR1_1           0x30    /* 过采样率: 1 */

/* 过采样率 (OSR2) */
#define QMC5883P_CTRL1_OSR2_1           0x00    /* 过采样率: 1 */
#define QMC5883P_CTRL1_OSR2_2           0x40    /* 过采样率: 2 */
#define QMC5883P_CTRL1_OSR2_4           0x80    /* 过采样率: 4 */
#define QMC5883P_CTRL1_OSR2_8           0xC0    /* 过采样率: 8 */

/* 数据输出速率 */
#define QMC5883P_CTRL1_ODR_10HZ         0x00    /* 输出速率: 10Hz */
#define QMC5883P_CTRL1_ODR_50HZ         0x04    /* 输出速率: 50Hz */
#define QMC5883P_CTRL1_ODR_100HZ        0x08    /* 输出速率: 100Hz */
#define QMC5883P_CTRL1_ODR_200HZ        0x0C    /* 输出速率: 200Hz */

/* ========================================================================
 * 控制寄存器 2 位定义
 * ======================================================================== */

#define QMC5883P_CTRL2_SOFT_RESET       0x80    /* 软复位位 */
#define QMC5883P_CTRL2_SELF_TEST        0x40    /* 自检 */
#define QMC5883P_CTRL2_RNG_2G           0x0C    /* 量程: ±2G */
#define QMC5883P_CTRL2_RNG_8G           0x08    /* 量程: ±8G */
#define QMC5883P_CTRL2_RNG_12G          0x04    /* 量程: ±12G */
#define QMC5883P_CTRL2_RNG_30G          0x00    /* 量程: ±30G */

/* ========================================================================
 * 灵敏度参数 (单位: LSB/Gauss)
 * ======================================================================== */

#define QMC5883P_SENS_2G                15000   /* ±2G 量程灵敏度 */
#define QMC5883P_SENS_8G                3750    /* ±8G 量程灵敏度 */
#define QMC5883P_SENS_12G               2500    /* ±12G 量程灵敏度 */
#define QMC5883P_SENS_30G               1000    /* ±30G 量程灵敏度 */

/* ========================================================================
 * 硬编码校准值 (手动校准后更新此处)
 * ======================================================================== */

#define MAG_X_OFFSET                    0.167200f
#define MAG_Y_OFFSET                    -0.906200f
#define MAG_Z_OFFSET                    -0.713000f
#define MAG_X_SCALE                     2.052229f
#define MAG_Y_SCALE                     0.711101f
#define MAG_Z_SCALE                     0.903787f

/* ========================================================================
 * 状态返回类型
 * ======================================================================== */

/**
 * @brief QMC5883P 操作状态枚举
 */
typedef enum {
    QMC5883P_OK = 0,        /* 操作成功 */
    QMC5883P_ERROR,         /* 操作失败 */
    QMC5883P_ERROR_ID       /* 芯片 ID 验证失败 */
} QMC5883P_Status_t;

/**
 * @brief 工作模式枚举
 */
typedef enum {
    QMC5883P_MODE_SUSPEND   = QMC5883P_CTRL1_MODE_SUSPEND,    /* 挂起模式 */
    QMC5883P_MODE_NORMAL    = QMC5883P_CTRL1_MODE_NORMAL,     /* 正常模式 */
    QMC5883P_MODE_SINGLE    = QMC5883P_CTRL1_MODE_SINGLE,     /* 单次测量 */
    QMC5883P_MODE_CONTINUOUS = QMC5883P_CTRL1_MODE_CONT       /* 连续测量 */
} QMC5883P_Mode_t;

/**
 * @brief 数据输出速率枚举
 */
typedef enum {
    QMC5883P_ODR_10HZ   = QMC5883P_CTRL1_ODR_10HZ,    /* 10Hz */
    QMC5883P_ODR_50HZ   = QMC5883P_CTRL1_ODR_50HZ,    /* 50Hz */
    QMC5883P_ODR_100HZ  = QMC5883P_CTRL1_ODR_100HZ,   /* 100Hz */
    QMC5883P_ODR_200HZ  = QMC5883P_CTRL1_ODR_200HZ    /* 200Hz */
} QMC5883P_Spd_t;

/**
 * @brief 量程范围枚举
 */
typedef enum {
    QMC5883P_RNG_2G  = QMC5883P_CTRL2_RNG_2G,      /* ±2G */
    QMC5883P_RNG_8G  = QMC5883P_CTRL2_RNG_8G,      /* ±8G */
    QMC5883P_RNG_12G = QMC5883P_CTRL2_RNG_12G,     /* ±12G */
    QMC5883P_RNG_30G = QMC5883P_CTRL2_RNG_30G      /* ±30G */
} QMC5883P_Rng_t;

/* ========================================================================
 * 设备结构定义
 * ======================================================================== */

/**
 * @brief QMC5883P 设备数据结构体
 * 
 * 保存设备配置参数、校准偏移和缩放系数、以及最新的磁场数据
 * 用户无需直接操作此结构体，由 QMC5883P_Init() 自动初始化
 */
typedef struct {
    /* 配置参数 */
    uint8_t mode;               /* 工作模式 */
    uint8_t speed;              /* 数据输出速率 */
    uint8_t range;              /* 量程范围 */
    uint16_t sensitivity;       /* 当前量程对应的灵敏度 */
    
    /* 校准偏移 (硬铁补偿) */
    float offset_x;
    float offset_y;
    float offset_z;
    
    /* 校准缩放 (软铁补偿) */
    float scale_x;
    float scale_y;
    float scale_z;
    
    /* 最新磁场数据 (单位: Gauss) */
    float mag_x;
    float mag_y;
    float mag_z;
} QMC5883P_Device_t;

/* ========================================================================
 * 函数声明
 * ======================================================================== */

/**
 * @brief 初始化 QMC5883P 设备
 * 
 * 包含完整的初始化流程:
 *   1. 初始化设备结构体参数
 *   2. 初始化软件 I2C 引脚 (PB12/PB13/PB14)
 *   3. 硬复位芯片并验证芯片 ID
 *   4. 设置量程范围和运行参数
 * 
 * @param dev   指向 QMC5883P_Device_t 结构体的指针
 * @param mode  工作模式 (QMC5883P_Mode_t)
 * @param speed 数据输出速率 (QMC5883P_Spd_t)
 * @param range 量程范围 (QMC5883P_Rng_t)
 */
void QMC5883P_Init(QMC5883P_Device_t *dev, QMC5883P_Mode_t mode,
                   QMC5883P_Spd_t speed, QMC5883P_Rng_t range);

/**
 * @brief 启动传感器 (硬件初始化)
 * 
 * 执行以下4个步骤:
 *   1. 软复位芯片
 *   2. 验证芯片 ID
 *   3. 设置量程范围
 *   4. 设置运行参数
 * 
 * @param dev 指向 QMC5883P_Device_t 结构体的指针
 * @return QMC5883P_Status_t 操作状态
 */
uint8_t QMC5883P_Begin(QMC5883P_Device_t *dev);

/**
 * @brief 更新磁场数据
 * 
 * 等待数据就绪后从传感器读取最新数据
 * 经校准转换后存入 dev->mag_x/y/z 中
 * 
 * @param dev 指向 QMC5883P_Device_t 结构体的指针
 * @return QMC5883P_Status_t 操作状态
 */
uint8_t QMC5883P_Update(QMC5883P_Device_t *dev);

/**
 * @brief 获取 X 轴数据
 */
static inline float QMC5883P_GetX(QMC5883P_Device_t *dev) { return dev->mag_x; }

/**
 * @brief 获取 Y 轴数据
 */
static inline float QMC5883P_GetY(QMC5883P_Device_t *dev) { return dev->mag_y; }

/**
 * @brief 获取 Z 轴数据
 */
static inline float QMC5883P_GetZ(QMC5883P_Device_t *dev) { return dev->mag_z; }

/**
 * @brief 硬铁校准
 * 
 * 当 trigger 为真时进入校准模式，旋转设备采集各轴最大最小值
 * 校准完成后根据椭球拟合计算偏移和缩放系数
 * 
 * 注意: 校准完成后需将计算出的 MAG_* 宏更新到本文件头部
 * 校准结果会自动写入设备结构体中的 offset 和 scale 字段
 * 
 * @param dev    指向 QMC5883P_Device_t 结构体的指针
 * @param trigger 校准触发 (1=启动/继续校准, 0=结束校准并保存结果)
 */
void QMC5883P_Calibration(QMC5883P_Device_t *dev, uint8_t trigger);

#endif /* __BSP_QMC5883P_H */
