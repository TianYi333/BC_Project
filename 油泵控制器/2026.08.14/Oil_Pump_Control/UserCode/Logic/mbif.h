/*
 * mbif.h
 *
 *  Created on: Oct 15, 2025
 *      Author: 28038
 */

#ifndef INC_MBIF_H_
#define INC_MBIF_H_

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "mb.h"
#include "mbport.h"

#define mbif_task_stk_size     1024*4
#define mbif_task_prio          osPriorityNormal2  


/**
 * modbus的任务句柄
 */
extern TaskHandle_t MBif_task_handler;


/**
 * @fn void start_user_interface_task(void)
 * @brief modbus接口任务
 *
 */
void MBif_task();
void start_mbif_task();
uint32_t GetModbusBaudBySel(uint8_t sel);


#endif /* INC_MBIF_H_ */
