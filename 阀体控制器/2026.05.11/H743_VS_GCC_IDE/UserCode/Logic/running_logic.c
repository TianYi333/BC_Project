/*
 * running_logic.c
 *
 *  Created on: Sep 26, 2025
 *      Author: 28038
 */

#include "running_logic.h"
#include "Msg_Queue.h"
#include "oled_task.h"
#include "rtc_clock.h"
#include "timers.h"
#include "log.h"
#include <stdio.h>

/**
 * 电机位置列表
 */
//_MOTOR_LOC motor_loc_list[21] = { { 0, 0, 0 }, { 1, 45, 2 }, { 2, 90, 0 }, { 3,
//		135, 1 }, { 4, 180, 0 }, { 5, 225, 2 }, { 6, 270, 0 }, { 7, 315, 1 }, {
//		8, 360, 0 }, { 9, 405, 2 }, { 10, 450, 0 }, { 11, 495, 1 },
//		{ 12, 540, 0 }, { 13, 585, 2 }, { 14, 630, 0 }, { 15, 675, 1 }, { 16,
//				720, 0 }, { 17, 765, 2 }, { 18, 810, 0 }, { 19, 855, 1 }, { 20,
//				900, 0 } };
/**
 * 六代阀电机位置表,六代阀步进角度为90度，修改为如下定位方式
 */
_MOTOR_LOC motor_loc_list[12] = {{0, 0, 2}, {1, 90, 0}, {2, 180, 1}, {3, 270, 0}, {4, 360, 2}, {5, 360 + 90, 0}, {6, 360 + 180, 1}, {7, 360 + 270, 0}, {8, 720, 2}, {9, 720 + 90, 0}, {10, 720 + 180, 1}, {11, 720 + 270, 0}};

/**
 * 六代阀P模块定位
 */
_MOTOR_LOC motor_loc_list_p[12] = {{0, 0, 0}, {1, 45, 2}, {2, 90, 0}, {3, 135, 1}, {4, 180, 0}, {5, 225, 2}, {6, 270, 0}, {7, 315, 1}, {8, 360, 0}};
/**
 * 主要系统状态
 */
_SYS_STATUS main_sys_status;
/**
 * 任务对象列表，测试运行默认写入最后一个，自动任务默认顺序写入
 */
_TASK_OBJECT task_object_list[5] = {{.task_status = 0, .inject_id = 0, .val = 0}, {.task_status = 0, .inject_id = 0, .val = 0}, {.task_status = 0, .inject_id = 0, .val = 0}, {.task_status = 0, .inject_id = 0, .val = 0}, {.task_status = 0, .inject_id = 0, .val = 0}};

/**
 * 注油器任务的句柄
 */
TaskHandle_t injector_task_handler;

TimerHandle_t xTimeHandle[injector_count];

/**
 * 注油任务的启动参数
 */
const osThreadAttr_t injector_task_attributes = {.name = "injector_task",
												 .stack_size = injector_task_stk_size,
												 .priority = injector_task_prio};

/**
 * @fn void start_injector_task(void)
 * @brief 启动注油器任务
 *
 */
void start_injector_task(void)
{
	injector_task_handler = osThreadNew(injector_task, NULL,
										&injector_task_attributes);
}

/**
 * @fn void injector_task()
 * @brief 注油器的任务函数
 *
 */
void injector_task(void *argument)
{
	(void)argument; // 消除未使用警告
	/**
	 * 系统初始化，进行电机复位
	 */
	uint8_t task_id = 0;
	if (zdt_run_motor_zero(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg) != 1)
	{
		main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
		logInfo("Q reset failure .");
		show_oled_msg(OLED_MSG_ERROR, "Q reset failure .");
	}
	if (zdt_run_motor_zero(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg) != 1)
	{
		main_sys_status.sys_warning = P_MODEL_RESET_FAILURE;
		logInfo("P reset failure .");
		show_oled_msg(OLED_MSG_ERROR, "P reset failure .");
	}
	vTaskDelay(3000);
	/**
	 * 进入任务循环
	 */
	while (1)
	{
		/**
		 * 通过系统状态选择执行动作
		 */
		switch (main_sys_status.running_mode)
		{
		case SYS_MODE_STARTING:
			/**
			 * 如果系统状态为初始化，则进行电机预备位置初始化。
			 */
			show_oled_msg(OLED_MSG_UPDATE, "motor init ...");
			main_sys_status.is_Busy = 1;
			start_running();
			vTaskDelay(2000);
			main_sys_status.is_Busy = 0;
			break;
			/**
			 * 如果系统状态为准备好，则扫描任务列表，执行任务。
			 */
		case SYS_MODE_READY:
			for (task_id = 0;
				 task_id < (sizeof(task_object_list) / sizeof(task_object_list[0])); task_id++)
			{
				if (task_object_list[task_id].task_status == 1)
				{
					/**
					 * 执行任务列表里面的任务，并显示提示信息
					 */
					main_sys_status.is_Busy = 1;
					show_oled_msg(OLED_MSG_UPDATE, "run task %d", task_id);
					if (inject_running(task_object_list[task_id].val,
									   task_object_list[task_id].inject_id) == 1)
					{
						main_sys_status.injector[task_object_list[task_id].inject_id - 1].injectRequest = 66;
						main_sys_status.injector[task_object_list[task_id].inject_id - 1].tagTime = Time_To_Unix() * 1000;
						task_object_list[task_id].task_status = 0;
					}
					else
					{
						main_sys_status.injector[task_object_list[task_id].inject_id - 1].injectRequest = 99;
						main_sys_status.injector[task_object_list[task_id].inject_id - 1].tagTime = Time_To_Unix() * 1000;
						task_object_list[task_id].task_status = 0;
					}

					main_sys_status.is_Busy = 0;
				}
			}

			break;
		case SYS_MODE_MONITOR:
			for (task_id = 0;
				 task_id < (sizeof(task_object_list) / sizeof(task_object_list[0])); task_id++)
			{
				if (task_object_list[task_id].task_status == 1)
				{
					/**
					 * 执行任务列表里面的任务，并显示提示信息
					 */
					main_sys_status.is_Busy = 1;
					show_oled_msg(OLED_MSG_UPDATE, "run task %d", task_id);
					inject_running(task_object_list[task_id].val,
								   task_object_list[task_id].inject_id);
					task_object_list[task_id].task_status = 0;
					main_sys_status.is_Busy = 0;
				}
			}
			break;
		case SYS_MODE_SETTING:
			break;
		case SYS_MODE_AUTO:
			break;
		}

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}
/**
 * @fn void injector_timer()
 * @brief 创建注油器定时器
 *
 */
void injector_timer_init()
{
	uint8_t tag;
	for (tag = 0; tag < injector_count; tag++)
	{
		xTimeHandle[tag] = xTimerCreate("injector", 30 * 1000, pdTRUE,
										&main_sys_status.injector[tag].injector_id,
										(TimerCallbackFunction_t)injector_task_maker);
	}
}
/**
 * @fn void injector_task_maker(TimerHandle_t)
 * @brief 定时任务请求函数（注油器定时器的回调）
 *
 * @param xTimer
 */
void injector_task_maker(TimerHandle_t xTimer)
{
	uint16_t *id = 0;
	id = (uint16_t *)pvTimerGetTimerID(xTimer);
	if (id > 0)
	{
		main_sys_status.injector[*id - 1].injectRequest = 99;
	}
}

/**
 * @fn int8_t start_sys_status(void)
 * @brief 初始化系统状态
 *
 */
void init_sys_status(void)
{
	main_sys_status.running_mode = SYS_MODE_STARTING;
	main_sys_status.sys_warning = SYS_RUNNING;
	main_sys_status.sys_start = 0;
	main_sys_status.p_model.point = motor_loc_list[0];
	main_sys_status.q_model.point = motor_loc_list[0];
	main_sys_status.q_model.sen = 0;
	main_sys_status.q_model.dir = 1;
	main_sys_status.is_Monitor = 1;
	main_sys_status.slaveID = 1;
	main_sys_status.is_Busy = 1;

	uint8_t tag;
	for (tag = 0; tag < injector_count; tag++)
	{
		main_sys_status.injector[tag].injector_id = tag + 1;
	}
}

/**
 * @fn int8_t start_running(void)
 * @brief 初始化P模块电机，进入预备状态
 *
 */
uint8_t start_running(void)
{

	/**
	 * 先进行P模块的自检,确保Q模块自检时油路通畅
	 */
	if (p_motor_run_point(motor_loc_list_p[1]) != 1)
	{
		main_sys_status.sys_warning = P_MODEL_RESET_FAILURE;
		return 0;
	}
	/**
	 * 进行Q模块复位，检查传感器位置和电机位置，判断是否需要进行阀芯复位
	 */
	if (Read_MET_Sensor() == 0)
	{
		/**
		 * 如果Q模块传感器不在限位，则转动到0点等待传感器到限位
		 */
		if (q_motor_run_point(motor_loc_list[0]) == 1)
		{
			/**
			 * 电机转到开放角度，等待阀芯传感器信号
			 */
			// printf("waiting sen action...");
			show_oled_msg(OLED_MSG_UPDATE, "waiting sen ...");
			/**
			 * 强制确认传感器位置
			 */
			if (waiting_met_sen(motor_loc_list[0].pipe) > 0)
			{
				/**
				 * 传感器位置正确，记录传感器位置
				 */
				main_sys_status.q_model.sen = Read_MET_Sensor();
				/**
				 * 电机运行到运行预备点
				 */
				if (q_motor_run_point(motor_loc_list[1]) != 1)
				{
					/**
					 * 电机运行到预备点失败，报错返回
					 */
					main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
					main_sys_status.running_mode = SYS_MODE_STARTING;
					show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
					return 0;
				}
				else
				{
					/**
					 * 电机运行到预备点成功，返回
					 */
					show_oled_msg(OLED_MSG_UPDATE, "p ready to %d", motor_loc_list[2].id);

					// printf("motor ready to %d \n", motor_loc_list[2].id);
					main_sys_status.running_mode = SYS_MODE_READY;
					return 1;
				}
			}
			else
			{
				/**
				 * 传感器确认失败，报错返回
				 */
				main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
				main_sys_status.running_mode = SYS_MODE_STARTING;
				show_oled_msg(OLED_MSG_ERROR, "init sen error ...");
				return 0;
			}
		}
		else
		{
			main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
			main_sys_status.running_mode = SYS_MODE_STARTING;
			show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
			return 0;
		}
	}
	else
	{
		/**
		 * 检查传感器位置和电机角度是否对应，当前电机位于初始位置，如果传感器位置与1点重叠，则需要运动到2点作为运行预位
		 */
		if (Read_MET_Sensor() == motor_loc_list[0].pipe)
		{
			if (q_motor_run_point(motor_loc_list[1]) != 1)
			{
				main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
				main_sys_status.running_mode = SYS_MODE_STARTING;
				show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
				return 0;
			}
			// printf("motor ready to %d \n", motor_loc_list[2].id);
			show_oled_msg(OLED_MSG_UPDATE, "p ready to %d", motor_loc_list[2].id);

			// printf("motor ready to %d \n", motor_loc_list[2].id);
			main_sys_status.running_mode = SYS_MODE_READY;
			return 1;
		}
		else
		{
			if (q_motor_run_point(motor_loc_list[3]) != 1)
			{
				main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
				main_sys_status.running_mode = SYS_MODE_STARTING;
				show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
				return 0;
			}
			// printf("motor ready to %d \n", motor_loc_list[3].id);
			show_oled_msg(OLED_MSG_UPDATE, "p ready to %d", motor_loc_list[3].id);

			// printf("motor ready to %d \n", motor_loc_list[3].id);
			main_sys_status.running_mode = SYS_MODE_READY;
			return 1;
		}
	}
}
/**
 * @fn uint8_t inject_running(uint8_t, uint8_t)
 * @brief 注油运行
 *
 * @param inject_val   注入量
 * @param injector_id  出口ID
 * @return
 */
uint8_t inject_running(uint8_t inject_val, uint8_t injector_id)
{
	uint8_t tag = 0;
	main_sys_status.injector[injector_id - 1].executionVol = inject_val;
	show_oled_msg(OLED_MSG_UPDATE, "task %d-%d", injector_id, inject_val);
	/**
	 * 判断Q模块位置是不是关闭点
	 */
	if (q_motor_run_close() == 0)
	{
		show_oled_msg(OLED_MSG_ERROR, "motor error ...");
		main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
		return 0;
	}
	/**
	 * 先选择出油口，再进行供油，完成后P模块复位
	 */
	if (p_motor_run_point(motor_loc_list_p[injector_id - 1]) == 0)
	{
		main_sys_status.sys_warning = P_MODEL_RESET_FAILURE;
		show_oled_msg(OLED_MSG_ERROR, "P motor error ...");
		// P模块运行失败的处理
		return 0;
	}

	for (tag = inject_val; tag > 0; tag--)
	{
		/**
		 * Q电机从预备点运行到下一个开放点
		 */
		if (q_motor_run_open() != 1)
		{
			/**
			 * 电机运行失败，任务失败返回。
			 */
			show_oled_msg(OLED_MSG_ERROR, "motor error ...");
			main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
			vTaskDelay(2000);
			return 0;
		}
		/**
		 * 电机转到开放角度，等待阀芯传感器信号
		 */
		printf("met sen go %d \n", main_sys_status.q_model.point.pipe);

		if (waiting_met_sen(main_sys_status.q_model.point.pipe) == 0)
		{
			/**
			 * 没有收到金属传感器中断
			 */
			printf("met sen error \n");
			show_oled_msg(OLED_MSG_ERROR, "met sen error");
			main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
			return 0;
		}
		main_sys_status.injector[injector_id - 1].executionVol = tag - 1;
		show_oled_msg(OLED_MSG_UPDATE, "task %d-%d", injector_id, tag - 1);
		// vTaskDelay(100);
	}
	/**
	 * 循环内部不在关闭位置停止，单点注油完成后回关闭位置
	 */
	if (q_motor_run_close() == 0)
	{
		show_oled_msg(OLED_MSG_ERROR, "motor error ...");
		main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
		return 0;
	}
	return 1;
}

/**
 * @fn int8_t p_motor_run_point(_MOTOR_LOC)
 * @brief P阀电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t p_motor_run_point(_MOTOR_LOC point)
{
	if (zdt_run_motor_loc_abs_t(P_MOTOR_ADDR, P_MOTOR_IF, msgQueue_ID_CAN1,
								P_MOTOR_RATE, point.deg) == 1)
	{
		main_sys_status.p_model.point = point;
		main_sys_status.p_model.status = MOTOR_STATUS_READY;
		return 1;
	}
	else
	{
		show_oled_msg(OLED_MSG_ERROR, "P running failure .");
		main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
		main_sys_status.p_model.status = MOTOR_STATUS_PROTECTED;
		show_oled_msg(OLED_MSG_ERROR, "P motor release protect .");
		// 进行解除保护操作
		zdt_release_motor_protect(P_MOTOR_ADDR, P_MOTOR_IF, msgQueue_ID_CAN1);
		// 再次尝试运行到指定位置
		if (zdt_run_motor_loc_abs_t(P_MOTOR_ADDR, P_MOTOR_IF, msgQueue_ID_CAN1,
									P_MOTOR_RATE, point.deg) == 1)
		{
			main_sys_status.p_model.point = point;
			main_sys_status.p_model.status = MOTOR_STATUS_READY;
			return 1;
		}
		else
		{
			show_oled_msg(OLED_MSG_ERROR, "P running failure .");
			main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
			main_sys_status.p_model.status = MOTOR_STATUS_OFFLINE;
			return 0;
		}
	}
}

/**
 * @fn int8_t q_motor_run_point(_MOTOR_LOC)
 * @brief Q电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t q_motor_run_point(_MOTOR_LOC point)
{
	if (zdt_run_motor_loc_abs_t(Q_MOTOR_ADDR, Q_MOTOR_IF, msgQueue_ID_CAN2,
								Q_MOTOR_RATE, point.deg) == 1)
	{
		printf("Q motor run to N.%d deg.%d sen.%d\n", point.id, point.deg, point.pipe);
		main_sys_status.q_model.point = point;
		main_sys_status.q_model.motor_status = MOTOR_STATUS_READY;
		return 1;
	}
	else
	{
		printf("Q motor err to N.%d deg.%d sen.%d\n", point.id, point.deg, point.pipe);
		main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
		main_sys_status.q_model.motor_status = MOTOR_STATUS_PROTECTED;
		show_oled_msg(OLED_MSG_ERROR, "Q motor release protect .");

		// 进行解除保护操作
		zdt_release_motor_protect(Q_MOTOR_ADDR, Q_MOTOR_IF, msgQueue_ID_CAN2);
		// 再次尝试运行到指定位置
		if (zdt_run_motor_loc_abs_t(Q_MOTOR_ADDR, Q_MOTOR_IF, msgQueue_ID_CAN2,
									Q_MOTOR_RATE, point.deg) == 1)
		{
			main_sys_status.q_model.point = point;
			main_sys_status.q_model.motor_status = MOTOR_STATUS_READY;
			return 1;
		}
		else
		{
			show_oled_msg(OLED_MSG_ERROR, "Q running failure .");
			main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
			main_sys_status.q_model.motor_status = MOTOR_STATUS_OFFLINE;
			return 0;
		}
	}
}
/**
 * @fn int8_t q_motor_run_close()
 * @brief Q模块运行到关闭点
 *
 * @return
 */
int8_t q_motor_run_close()
{
	/**
	 * 判断Q模块是不是在关闭位置
	 */
	if (main_sys_status.q_model.point.pipe != 0)
	{
		/**
		 * 判断是否到达反转点
		 */
		switch (main_sys_status.q_model.point.id)
		{
		case 8:
			main_sys_status.q_model.dir = -1;
			break;
		case 2:
			main_sys_status.q_model.dir = 1;
			break;
		default:
			break;
		}
		/**
		 * Q模块运行到下一点，即关闭点。
		 */
		if (q_motor_run_point(
				motor_loc_list[main_sys_status.q_model.point.id + main_sys_status.q_model.dir]) != 1)
		{
			show_oled_msg(OLED_MSG_ERROR, "motor error ...");
			main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
			return 0;
		}
		else
		{
			return 1;
		}
	}
	else
	{
		return 1;
	}
}
/**
 * @fn int8_t q_motor_run_open()
 * @brief  Q模块运行到开放点
 *
 * @return
 */
int8_t q_motor_run_open()
{
	/**
	 * 判断Q模块是不是在关闭位置
	 */
	if (main_sys_status.q_model.point.pipe != 0)
	{
		/**
		 * 判断是否到达反转点
		 */
		switch (main_sys_status.q_model.point.id)
		{
		case 8:
			main_sys_status.q_model.dir = -1;
			break;
		case 2:
			main_sys_status.q_model.dir = 1;
			break;
		default:
			break;
		}
		/**
		 * Q模块运行到下一个开放点。
		 */
		if (q_motor_run_point(
				motor_loc_list[main_sys_status.q_model.point.id + (main_sys_status.q_model.dir * 2)]) != 1)
		{
			show_oled_msg(OLED_MSG_ERROR, "motor error ...");
			main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
			return 0;
		}
		else
		{
			return 1;
		}
	}
	else
	{
		/**
		 * Q模块运行到下一点。
		 */
		if (q_motor_run_point(
				motor_loc_list[main_sys_status.q_model.point.id + main_sys_status.q_model.dir]) != 1)
		{
			show_oled_msg(OLED_MSG_ERROR, "motor error ...");
			main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
			return 0;
		}
		else
		{
			return 1;
		}
	}
}

/**
 * @fn uint8_t Read_Sensor(void)
 * @brief 读取阀芯限位传感器，如果两个传感器都无效则输出0
 *
 * @return
 */
int8_t Read_MET_Sensor(void)
{
	if (READ_MET_SEN_1 == GPIO_PIN_SET)
	{
		if (READ_MET_SEN_2 == GPIO_PIN_RESET)
		{
			/**
			 * 传感器状态写入寄存器
			 */
			main_sys_status.q_model.sen = 1;
			main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
			return 1;
		}
		else
		{
			/**
			 * 如果两个传感器都有效，设备故障
			 */
			main_sys_status.q_model.sen_status = SENSOR_STATUS_D_OFFLINE;
			return 0;
		}
	}
	else
	{
		if (READ_MET_SEN_2 == GPIO_PIN_RESET)
		{
			/**
			 * 如果两个传感器都无效，阀芯没有复位或者设备故障
			 */

			return 0;
		}
		else
		{
			/**
			 * 传感器状态写入寄存器
			 */
			main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
			main_sys_status.q_model.sen = 2;
			return 2;
		}
	}
}

/**
 * @fn uint8_t waiting_met_sen(uint8_t)
 * @brief 强制确认传感器位置（直接读取传感器，如果读取确认失败就等待传感器中断）
 *
 * @param sen_id 需要确认的传感器ID
 * @return
 */
uint8_t waiting_met_sen(uint8_t sen_id)
{
	uint8_t action_sen_id;
	uint32_t tick_s = HAL_GetTick();
	printf("waiting sen to %d \n", sen_id);
	show_oled_msg(OLED_MSG_UPDATE, "waiting sen to %d \n", sen_id);
	/**
	 * 进入循环，接收中断消息
	 */
	while (sen_id > 0)
	{
		/**
		 * 先读取传感器，查看是不是提前置位
		 */
		if (Read_MET_Sensor() == sen_id)
		{
			printf("sen %d OK!\n", sen_id);
			main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
			return 1;
		}
		/**
		 * 如果读取传感器未到达置位，则等待中断消息
		 */
		if (xQueueReceive(QUEUE_MET_SEN, &action_sen_id,
						  pdMS_TO_TICKS(2000)) == pdPASS)
		{
			if (action_sen_id == sen_id)
			{
				if (Read_MET_Sensor() == sen_id)
				{
					printf("sen %d OK!\n", sen_id);
					main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
					return 1;
				}
			}
		}
		else
		{
			if (Read_MET_Sensor() == sen_id)
			{
				printf("sen %d OK!\n", sen_id);
				main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
				return 1;
			}
			else
			{
				printf("sen %d timeout\n", sen_id);
				show_oled_msg(OLED_MSG_UPDATE, "sen %d timeout\n", sen_id);
				if (sen_id == 1)
				{
					main_sys_status.q_model.sen_status = SENSOR_STATUS_T_OFFLINE;
				}
				else
				{
					main_sys_status.q_model.sen_status = SENSOR_STATUS_D_OFFLINE;
				}

				return 0;
			}
		}
		if ((tick_s + SYS_INJECT_TIMEOUT) < HAL_GetTick())
		{

			printf("sen %d timeout\n", sen_id);
			show_oled_msg(OLED_MSG_UPDATE, "sen %d timeout\n", sen_id);
			if (sen_id == 1)
			{
				main_sys_status.q_model.sen_status = SENSOR_STATUS_T_OFFLINE;
			}
			else
			{
				main_sys_status.q_model.sen_status = SENSOR_STATUS_D_OFFLINE;
			}

			return 0;
		}
	}
	return 0;
}
