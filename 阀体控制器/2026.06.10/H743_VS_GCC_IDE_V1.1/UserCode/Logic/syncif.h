/*
 * syncif.h
 *
 *  Created on: Oct 16, 2025
 *      Author: 28038
 */

#ifndef INC_SYNCIF_H_
#define INC_SYNCIF_H_

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "running_logic.h"
#include "mbReg.h"


#define regID_IsRunning        200
#define regID_DateTime         203
#define regID_SlaveID          201
#define regID_Injector         300

#define syncif_task_stk_size     1024*8
#define syncif_task_prio         osPriorityNormal3

/**
 * 数据同步的任务句柄
 */

extern TaskHandle_t syncif_task_handler;

/**
 * @fn void start_syncif_task(void)
 * @brief 数据同步接口任务
 *
 */
void start_syncif_task();

/**
 * @fn void syncif_task()
 * @brief 数据同步任务主函数
 *
 */
void syncif_task(void *argument);
/**
 * @fn void sync_from_sysdb()
 * @brief 从数据库读取数据
 *
 */
void sync_from_sysdb();
/**
 * @fn void sync_to_sysdb()
 * @brief 同步数据到数据库
 *
 */
void sync_to_sysdb();
/**
 * @fn void sync_to_modbus()
 * @brief 同步数据到modbus寄存器数组
 *
 */
void sync_to_modbus();
/**
 * @fn void sync_from_modbus()
 * @brief 从modbus寄存器数组同步数据
 *
 */
void sync_from_modbus(_MB_REG mbMsg);
/**
 * @fn void sync_injector_timers()
 * @brief 同步定时器状态
 *
 */
void sync_injector_timers();
/**
 * @fn void syncif_task_cycle()
 * @brief 循环同步的任务函数
 *
 */
void syncif_task_cycle(void *argument);

int8_t init_sys_db();

#endif /* INC_SYNCIF_H_ */
