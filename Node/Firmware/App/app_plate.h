/****************************************************************************
 * 车牌子系统应用层 - app_plate.h
 *
 * 功能描述:
 *   节点侧车牌识别接入, 通过 USART3(PB10/PB11 @115200) 与 ESP32-S3
 *   摄像头模组通信:
 *   - 单命令通道三态状态机(拍照 AT+RUN / 探活 AT+INFO), 半双工口同时
 *     只允许一条在途命令, 全程非阻塞;
 *   - 解析 $PLATE 结果并缓存(PlateCache);
 *   - 自动触发判定(策略 + 车位上沿 + 冷却); 车离开时清空车牌缓存
 *   - 周期探活维护摄像头在线位。
 *
 * 作者: Bjmyhc
 * 日期: 2026-10-09 (v4 车牌子系统接入)
 ****************************************************************************/

#ifndef __APP_PLATE_H
#define __APP_PLATE_H

#include <stdint.h>
#include "lora_protocol.h"

/* ==================== 触发来源 ==================== */
#define PLATE_SRC_AUTO      1   /* 节点自动触发(策略命中) */
#define PLATE_SRC_MANUAL    2   /* 软件手动触发(AT+CAPTURE) */

/* ==================== 车牌缓存 ====================
 * 摄像头最近一次识别结果(事件量), 供 AT+PLATE 组帧取用 */
typedef struct {
    char     plate[24];   /* UTF-8 车牌; 未识别为 "-" */
    uint8_t  conf;        /* 置信度 0~100 (未识别恒为 0) */
    uint8_t  valid;       /* 0=未识别, 1=有效 */
    uint8_t  source;      /* 1=自动 2=手动 */
    uint32_t frameNo;     /* 摄像头帧号(仅排障, 不参与判新) */
    uint8_t  color;       /* 预留车牌颜色, 一期恒 0 */
    uint8_t  hasData;     /* 1=至少收到过一次结果(供上层判"从未上报") */
} PlateCache_t;

/* ==================== 跨文件共享全局 ====================
 * 实际定义在 app_plate.c */
extern PlateCache_t      g_plateCache;         /* 车牌缓存 */
extern volatile uint8_t  g_plateFetchPending;  /* 1=有新车牌待取(数据帧 CamFlags bit2) */
extern volatile uint8_t  g_camOnline;          /* 1=摄像头在线(CamFlags bit3) */
extern uint8_t           g_capturePolicy;      /* 拍照策略 0~3(CamFlags bit0~1) */
/* S5 车牌缩略图(94×24 二值 282B): 摄像头 AT+THUMB 推来 → CRC 校验缓存
 * → 数据帧 CamFlags bit4 立旗 → 网关 AT+IMG 取 → 节点拆 2 包回 0xF2 */
extern volatile uint8_t  g_imgFetchPending;    /* 1=有缩略图待网关取(CamFlags bit4) */
extern uint8_t           g_thumbCache[LORA_IMG_BYTES];  /* 缩略图位图(行优先, 1=白字) */
extern volatile uint8_t  g_thumbValid;         /* 1=缓存内有通过 CRC 校验的图 */
extern volatile uint16_t g_thumbNo;            /* 图像序号(透传摄像头计数, 网关丢残包) */

/* ==================== 接口函数 ==================== */

/****************************************************************************
 * 函数名: Plate_Init
 * 功能:   初始化车牌子系统(内含 Usart3_Init), 由 BSP_Init 调用
 * 参数:   无
 * 返回:   无
 ****************************************************************************/
void Plate_Init(void);

/****************************************************************************
 * 函数名: Plate_Task
 * 功能:   车牌子系统周期任务(非阻塞), 主循环每轮调用一次
 * 参数:   无
 * 返回:   无
 * 说明:   取 USART3 数据喂状态机 + 自动触发判定 + 查超时; 绝不阻塞
 ****************************************************************************/
void Plate_Task(void);

/****************************************************************************
 * 函数名: Plate_RequestCapture
 * 功能:   请求一次拍照(手动/自动共用入口), 置请求旗子, 由状态机择机发出
 * 参数:   src - PLATE_SRC_AUTO / PLATE_SRC_MANUAL
 * 返回:   无
 ****************************************************************************/
void Plate_RequestCapture(uint8_t src);

/****************************************************************************
 * 函数名: Plate_BuildFrame
 * 功能:   组装一帧车牌结果(LoraPlate_t)供 LoRa_Node_SendPlate 发送
 * 参数:   无
 * 返回:   静态结构体指针(内容为当前 g_plateCache 快照), crc16 由发送侧填
 ****************************************************************************/
const LoraPlate_t *Plate_BuildFrame(void);

#endif /* __APP_PLATE_H */
