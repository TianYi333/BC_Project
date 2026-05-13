/*
 * mbif.c
 *
 *  Created on: Oct 15, 2025
 *      Author: 28038
 */

#include "mbif.h"
#include "mbReg.h"
/**
 * modbus的任务句柄
 */
TaskHandle_t mbif_task_handler;
TaskHandle_t mb_tcp_if_task_handler;
/**
 * modbus任务参数
 */
const osThreadAttr_t mbif_task_attributes = { .name = "mbifTask", .stack_size =
mbif_task_stk_size, .priority = mbif_task_prio, };

/**
 * @fn void start_user_interface_task(void)
 * @brief modbus接口任务
 *
 */
void start_mbif_task() {
	mbif_task_handler = osThreadNew(mbif_task, NULL, &mbif_task_attributes);
}
/**
 * @fn void main_menu_go()
 * @brief modbus任务主函数
 *
 */
void mbif_task() {

	init_mb_reg();
	vTaskDelay(4000);
	eMBInit(MB_RTU, 0x01, 1, 115200, MB_PAR_NONE);
	eMBEnable();
	while (1) {
		if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {

			eMBPoll();
		}
//		eMBPoll();
//		vTaskDelay(5);
	}

}

//usRegHoldingBuf[REG_Holding_NREGS]

/**
 * @fn void init_mb_reg()
 * @brief modbus寄存器复位函数
 *
 */
void init_mb_reg() {
	taskENTER_CRITICAL();
	for (uint16_t i = 0; i < REG_Holding_NREGS; i++) {
		usRegHoldingBuf[i] = 0;
	}
	taskEXIT_CRITICAL();
}

void mb_tcp_if_task() {
	vTaskDelay(10000);
	eMBTCPInit(502);
	eMBEnable();
	while (1) {
//		if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
//
//			eth_eMBPoll();
//		}
		eMBPoll();
		vTaskDelay(5);
	}

}

void start_mb_tcp_if_task() {
	mbif_task_handler = osThreadNew(mb_tcp_if_task, NULL,
			&mbif_task_attributes);
}
