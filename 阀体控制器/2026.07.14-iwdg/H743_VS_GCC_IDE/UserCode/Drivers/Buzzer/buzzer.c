/*
 * buzzer.c
 *
 *  Created on: Sep 24, 2025
 *      Author: 28038
 */

#include "buzzer.h"

TaskHandle_t buzz_task_handler;   //任务句柄

const osThreadAttr_t Buzz_Task_attributes = { .name = "BuzzTask", .stack_size =
BUZZ_TASK_STK_SIZE, .priority = BUZZ_TASK_PRIO, };

/**
 * @fn void start_oled_Task(void)
 * @brief 启动蜂鸣器任务
 *
 */
void start_buzz_task(void) {
	buzz_task_handler = osThreadNew(buzz_task, NULL, &Buzz_Task_attributes);
}

/**
 * @fn void buzz_task(void)
 * @brief 蜂鸣器任务的主函数
 *
 */
void buzz_task(void *pvParameters) {
	uint8_t buzz_status = 0;
	while (1) {
		xQueueReceive(QUEUE_BUZZ, &buzz_status, portMAX_DELAY);
		switch (buzz_status) {
			case BUZZ_LONG:
				buzz_long();
				buzz_status = 0;
				break;
			case BUZZ_SHORT:
				buzz_short();
				buzz_status = 0;
				break;
			case BUZZ_LONG_RUN:
				buzz_long_run();
				break;
			case BUZZ_SHORT_RUN:
				buzz_short_run();
				break;
			default:
				break;
		}
		
	}

}

/**
 * @fn void buzz_long(void)
 * @brief 长响一次
 *
 */
void buzz_long(void) {
	buzz_set();
	vTaskDelay(500);
	buzz_reset();
}
/**
 * @fn void buzz_long_run(void)
 * @brief 持续长响
 *
 */
void buzz_long_run(void) {
	buzz_set();
	vTaskDelay(1000);
	buzz_reset();
	vTaskDelay(1000);
}
/**
 * @fn void buzz_short(void)
 * @brief 短响一次
 *
 */
void buzz_short(void) {
	buzz_set();
	vTaskDelay(20);
	buzz_reset();
}
/**
 * @fn void buzz_short_run(void)
 * @brief 持续短响
 *
 */
void buzz_short_run(void) {
	buzz_set();
	vTaskDelay(500);
	buzz_reset();
	vTaskDelay(500);
}
/**
 * @fn void buzz_contral(uint8_t)
 * @brief 发送蜂鸣器控制命令
 *
 * @param type 响铃类型
 */
void buzz_contral(uint8_t type) {

	xQueueOverwrite(QUEUE_BUZZ, &type);
}
