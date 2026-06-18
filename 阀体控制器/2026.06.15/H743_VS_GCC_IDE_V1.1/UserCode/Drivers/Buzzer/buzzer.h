/*
 * buzzer.h
 *
 *  Created on: Sep 24, 2025
 *      Author: 28038
 */

#ifndef BUZZER_BUZZER_H_
#define BUZZER_BUZZER_H_

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "main.h"

#include "msg_queue.h"

#define BUZZ_TASK_STK_SIZE          128*2   //任务栈大小
#define BUZZ_TASK_PRIO  osPriorityNormal  //任务优先级

extern TaskHandle_t buzz_task_handler;   //任务句柄

#define BUZZ_LONG         1
#define BUZZ_SHORT        2
#define BUZZ_LONG_RUN     3
#define BUZZ_SHORT_RUN    4
#define BUZZ_RUN          5
#define BUZZ_LONG_SHORT   6

#define BUZZ_PORT  BUZZER_GPIO_Port
#define BUZZ_PIN   BUZZER_Pin

#define buzz_set()   (HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_SET))
#define buzz_reset() (HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET))

/**
 * @fn void buzz_contral(uint8_t)
 * @brief 发送蜂鸣器控制命令
 *
 * @param type 响铃类型
 */
void buzz_contral(uint8_t type);
/**
 * @fn void start_oled_Task(void)
 * @brief 启动蜂鸣器任务
 *
 */
void start_buzz_task(void);
/**
 * @fn void buzz_task(void)
 * @brief 蜂鸣器任务的主函数
 *
 */
void buzz_task(void *pvParameters);
/**
 * @fn void buzz_long(void)
 * @brief 长响一次
 *
 */
void buzz_long(void);
/**
 * @fn void buzz_long_run(void)
 * @brief 持续长响
 *
 */
void buzz_long_run(void);
/**
 * @fn void buzz_short(void)
 * @brief 短响一次
 *
 */
void buzz_short(void);
/**
 * @fn void buzz_short_run(void)
 * @brief 持续短响
 *
 */
void buzz_short_run(void);

#endif /* BUZZER_BUZZER_H_ */
