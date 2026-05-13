/*
 * running_logic.h
 *  运行逻辑以任务的形式常驻，通过向任务列表写入信息触发执行
 *  Created on: Sep 26, 2025
 *      Author: 28038
 */

#ifndef INC_RUNNING_LOGIC_H_
#define INC_RUNNING_LOGIC_H_

#include "motor_zdt.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "timers.h"
#include "oled_task.h"
/**
 * @def READ_MET_SEN_1
 * @brief 读取金属接近传感器状态，共计八个金属接近传感器，每个定量阀使用两个
 *
 */
#define READ_MET_SEN_1       HAL_GPIO_ReadPin(DO_INPUT_0_GPIO_Port,DO_INPUT_0_Pin)
#define READ_MET_SEN_2       HAL_GPIO_ReadPin(DO_INPUT_1_GPIO_Port,DO_INPUT_1_Pin)

#define injector_count              8

/**
 * 系统错误代码
 */
#define SYS_RUNNING                 0x00
#define MOTOR_RUNNING_FAILURE       0x01
#define P_MODEL_RESET_FAILURE       0x02
#define Q_MODEL_RESET_FAILURE       0x03

/**
 * 运行逻辑中各种超时时间
 */
#define SYS_INJECT_TIMEOUT          20000


/**
 *  @enum sys_status
 *  @brief 系统状态枚举
 */
typedef enum sys_mode {
	SYS_MODE_STARTING,
	SYS_MODE_READY,
	SYS_MODE_AUTO,
	SYS_MODE_MONITOR,
	SYS_MODE_SETTING
} _SYS_MODE;

/**
 * @enmu motor_status
 * @brief 电机状态枚举
 */
typedef enum motor_status {
	MOTOR_STATUS_READY,
	MOTOR_STATUS_PROTECTED,
	MOTOR_STATUS_OFFLINE
} _MOTOR_STATUS;

/**
 * @enum sensor_status
 * @brief 传感器状态枚举
 */
typedef enum sensor_status {
	SENSOR_STATUS_READY,
	SENSOR_STATUS_T_OFFLINE,
	SENSOR_STATUS_D_OFFLINE
} _SENSOR_STATUS;


/**
 * 注油器任务的相关定义
 */
#define injector_task_stk_size     128*32
#define injector_task_prio         osPriorityHigh

extern TaskHandle_t injector_task_handler;
extern TimerHandle_t xTimeHandle[injector_count];

/**
 * @struct motor_loc
 * @brief 标志电机目标位置
 *
 */
typedef struct motor_loc {
	uint8_t id;
	int16_t deg;
	uint8_t pipe;
} _MOTOR_LOC;

/**
 * @struct p_model_status
 * @brief  p电机状态结构体
 *
 */
typedef struct p_model_status {
	_MOTOR_LOC point;
	_MOTOR_STATUS status;
} _P_MODEL_ST;
/**
 * @struct q_model_status
 * @brief q电机状态结构体
 *
 */
typedef struct q_model_status {
	_MOTOR_LOC point;
	uint8_t sen;
	int8_t dir;
	_MOTOR_STATUS motor_status;
	_SENSOR_STATUS sen_status;
} _Q_MODEL_ST;
/**
 * @struct injector_info
 * @brief 注油点信息的结构体
 *
 */
typedef struct injector_info {
	uint16_t injector_id;
	uint16_t volume;
	uint16_t interval;
	uint16_t executionTime;
	uint16_t executionVol;
	uint8_t injectRequest;
	uint64_t tagTime;
} _INJECTOR_INFO;
/**
 * @struct system_status
 * @brief 系统状态类型
 *
 */
typedef struct system_status {
	uint8_t sys_start;
	_P_MODEL_ST p_model;
	_Q_MODEL_ST q_model;
	_SYS_MODE running_mode;
	uint8_t sys_warning;
	uint8_t is_Monitor;
	uint8_t is_Busy;
	uint8_t slaveID;
	_INJECTOR_INFO injector[injector_count];
} _SYS_STATUS;

/**
 * @struct task_object
 * @brief 注油任务对象
 *
 */
typedef struct task_object {
	uint8_t task_status;
	uint8_t val;
	uint32_t inject_id;
} _TASK_OBJECT;

/**
 * 任务对象列表
 */
extern __attribute__((section("._DTC_FSRAM")))       _TASK_OBJECT task_object_list[5];

/**
 * 注油器状态列表，传递注油点信息
 */
extern __attribute__((section("._DTC_FSRAM")))       _INJECTOR_INFO injector_list[32];

/**
 * 电机位置列表
 */
extern _MOTOR_LOC motor_loc_list[12];

/**
 * 主要系统状态
 */
extern _SYS_STATUS main_sys_status;

/**
 * @fn uint8_t Read_Sensor(void)
 * @brief 读取阀芯限位传感器，如果两个传感器都无效则输出0
 *
 * @return
 */
int8_t Read_MET_Sensor(void);
/**
 * @fn int8_t p_motor_run_point(_MOTOR_LOC)
 * @brief P阀电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t p_motor_run_point(_MOTOR_LOC point);
/**
 * @fn int8_t q_motor_run_point(_MOTOR_LOC)
 * @brief Q电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t q_motor_run_point(_MOTOR_LOC point);

/**
 * @fn uint8_t inject_running(uint8_t, uint8_t)
 * @brief 注油运行
 *
 * @param inject_val   注入量
 * @param injector_id  出口ID
 * @return
 */
uint8_t inject_running(uint8_t inject_val, uint8_t injector_id);
/**
 * @fn int8_t start_running(void)
 * @brief 初始化P模块电机，进入准备状态
 *
 * @return
 */
uint8_t start_running(void);
/**
 * @fn int8_t start_sys_status(void)
 * @brief 初始化系统状态
 *
 * @return
 */
void init_sys_status(void);
/**
 * @fn uint8_t waiting_met_sen(uint8_t)
 * @brief 强制确认传感器位置（直接读取传感器，如果读取确认失败就等待传感器中断）
 *
 * @param sen_id 需要确认的传感器ID
 * @return
 */
uint8_t waiting_met_sen(uint8_t sen_id);
/**
 * @fn void injector_task()
 * @brief 注油器的任务函数
 *
 */
void injector_task();
/**
 * @fn void start_injector_task(void)
 * @brief 启动注油器任务
 *
 */
void start_injector_task(void);
/**
 * @fn int8_t q_motor_run_point(_MOTOR_LOC)
 * @brief Q电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t q_motor_run_point(_MOTOR_LOC point);
/**
 * @fn int8_t q_motor_run_close()
 * @brief Q模块运行到关闭点
 *
 * @return
 */
int8_t q_motor_run_close();
/**
 * @fn int8_t q_motor_run_open()
 * @brief  Q模块运行到开放点
 *
 * @return
 */
int8_t q_motor_run_open();
/**
 * @fn void injector_task_maker(TimerHandle_t)
 * @brief 定时任务请求函数（注油器定时器的回调）
 *
 * @param xTimer
 */
void injector_task_maker(TimerHandle_t xTimer);
/**
 * @fn void injector_timer()
 * @brief 创建注油器定时器
 *
 */
void injector_timer_init();

#endif /* INC_RUNNING_LOGIC_H_ */
