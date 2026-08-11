/****************************************************************************
 * 应用层周期任务函数接口 - app_tasks.h
 * 
 * 功能描述:
 *   声明主循环中周期调用的业务任务函数接口
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-25
 ****************************************************************************/

#ifndef __APP_TASKS_H
#define __APP_TASKS_H

void US_Task(void);
void QMC_Task(void);
void ParkingStatus_Check(void);
void LED_Task(void);
void LoRa_Task(void);

#endif /* __APP_TASKS_H */
