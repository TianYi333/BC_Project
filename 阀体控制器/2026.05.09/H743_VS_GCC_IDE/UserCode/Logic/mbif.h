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
#define mbif_task_prio         osPriorityNormal1

/**
 * modbus的任务句柄
 */
extern TaskHandle_t mbif_task_handler;
extern TaskHandle_t mb_tcp_if_task_handler;

/**
 * @fn void start_user_interface_task(void)
 * @brief modbus接口任务
 *
 */
void start_mbif_task();
void start_mb_tcp_if_task();
/**
 * @fn void main_menu_go()
 * @brief modbus任务主函数
 *
 */
void mbif_task();
void mb_tcp_if_task();
/**
 * @fn void init_mb_reg()
 * @brief modbus寄存器复位函数
 *
 */
void init_mb_reg();

#endif /* INC_MBIF_H_ */
