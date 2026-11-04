/**
    var_init.c
    @brief 变量初始化函数，主要用于初始化系统配置参数和注油点配置参数的初始值
           离线下载器对内存区域的数据不进行初始化，所以需要在程序中手动进行变量的初始化，确保系统配置参数和注油点配置参数在系统启动时具有合理的初始值，从而保证系统的稳定运行。
    Created on: Oct 15, 2025
*/


#ifndef INC_VAR_INIT_H_
#define INC_VAR_INIT_H_


#include "syncif.h"
#include "running_logic.h"




/*****************函数声明*****************/

/**
 * @fn void init_main_config()
 * @brief 配置初始化函数,设置系统配置参数和注油点配置参数的初始值
 *
 */
void init_main_config(void);

/**
 * @fn void init_injector_config()
 * @brief 注油点配置初始化函数,设置注油点配置参数的初始值
 *
 */
void init_injector_config(void);

/**
 * @fn void init_main_sys_state()
 * @brief 系统状态初始化函数,设置系统状态的初始值
 *
 */
void init_main_sys_state(void);

/**
 * @fn void init_task_object_list()
 * @brief 任务对象列表初始化函数，设置任务对象列表的初始值
 *
 */
void init_task_object_list(void);

/**
 * @fn void init_Q_motor_loc_list()
 * @brief Q电机位置列表初始化函数，设置Q电机位置列表的初始值
 *
 */
void init_Q_motor_loc_list(void);

/**
 * @fn void init_P_motor_loc_list()
 * @brief P电机位置列表初始化函数，设置P电机位置列表的初始值
 *
 */
void init_P_motor_loc_list(void);

/**
 * @fn void var_init()
 * @brief
 * 变量初始化函数，调用其他初始化函数进行系统配置参数、注油点配置参数、系统状态、任务对象列表、电机位置列表的初始化
 *
 */
void var_init(void);
/**
 * @fn void init_mb_reg()
 * @brief modbus寄存器复位函数
 *
 */
void init_mb_reg();

#endif /* INC_VAR_INIT_H_ */