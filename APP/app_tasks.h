/**
 * @file app_tasks.h
 * @brief 应用层周期任务函数接口
 * @note  主循环中周期调用的业务任务函数
 * @author Bjmyhc
 * @date 2026-07-25
 */

#ifndef __APP_TASKS_H
#define __APP_TASKS_H

void US_Task(void);
void QMC_Task(void);
void ParkingStatus_Check(void);
void LED_Task(void);
void OLED_Task(void);
void GenerateParkingData(void);
void Wifi_Task(void);

#endif /* __APP_TASKS_H */
