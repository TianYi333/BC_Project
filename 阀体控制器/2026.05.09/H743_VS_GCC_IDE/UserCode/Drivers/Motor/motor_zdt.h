/*
 * motor_zdt.h
 *
 *  Created on: Sep 25, 2025
 *      Author: 28038
 */

#ifndef MOTOR_MOTOR_ZDT_H_
#define MOTOR_MOTOR_ZDT_H_

#include "string.h"
#include "bsp_fdcan.h"

/**
 * @def BigtoLittle32
 * @brief 大小端转换的宏定义
 *
 */
#define BigtoLittle32(A)   ((( (uint32_t)(A) & 0xff000000) >> 24) | \
                                       (( (uint32_t)(A) & 0x00ff0000) >> 8)   | \
                                       (( (uint32_t)(A) & 0x0000ff00) << 8)   | \
                                       (( (uint32_t)(A) & 0x000000ff) << 24))

#define zdt_p_msg msgQueue_ID_CAN1
#define zdt_q_msg msgQueue_ID_CAN2

#define Q_MOTOR_IF          2
#define P_MOTOR_IF          1

#define Q_MOTOR_ADDR        1
#define P_MOTOR_ADDR        1

#define Q_MOTOR_RATE        1
#define P_MOTOR_RATE        1

#define MOTOR_RUN_CCW     0
#define MOTOR_RUN_CW      1

/**
 * @fn uint32_t zdt_get_motor_loc_abs(uint16_t, uint8_t, QueueHandle_t, uint8_t)
 * @brief 获取电机的相对位置
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信地址
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @return 电机的运行结果
 */
uint32_t zdt_get_motor_loc_abs(uint16_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate);

/**
 * @fn int8_t zdt_run_motor_loc_abs(uint8_t, uint8_t, QueueHandle_t, uint8_t, int16_t)
 * @brief 电机运行到指定位置（绝对位置）
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @param loc 电机运动的目标位置
 * @return 电机运行结果
 */
int8_t zdt_run_motor_loc_abs(uint8_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate, int16_t loc);
/**
 * @fn int8_t zdt_run_motor_loc_abs_t(uint8_t, uint8_t, QueueHandle_t, uint8_t, int16_t)
 * @brief 电机运行到指定位置（绝对位置）柔性控制
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @param loc 电机运动的目标位置
 * @return 电机运行结果
 */
int8_t zdt_run_motor_loc_abs_t(uint8_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate, int16_t loc);

/**
 * @fn int8_t zdt_run_motor_loc_rel(uint8_t, uint8_t, QueueHandle_t, uint8_t, int16_t, uint8_t)
 * @brief 电机按相对位置运行
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @param deg 电机运行的相对角度
 * @param dir 电机的运行角度
 * @return 电机的运行结果
 */
int8_t zdt_run_motor_loc_rel(uint8_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate, int16_t deg,
		uint8_t dir);

/**
 * @fn uint8_t zdt_set_motor_reset_zero(uint8_t, uint8_t, QueueHandle_t)
 * @brief 电机直接置零，设置当前位置为零点。
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_set_motor_reset_zero(uint8_t addr, uint8_t interface, QueueHandle_t Queue);

/**
 * @fn uint8_t zdt_run_motor_zero(uint8_t, uint8_t, QueueHandle_t)
 * @brief 触发电机自动归零
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_run_motor_zero(uint8_t addr, uint8_t interface, QueueHandle_t Queue);

/**
 * @fn uint8_t zdt_set_motor_ma(uint8_t, uint8_t, QueueHandle_t)
 * @brief 设置电机闭环模式最大电流
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_set_motor_ma(uint8_t addr, uint8_t interface, QueueHandle_t Queue);
/**
 * @fn uint8_t zdt_release_motor_protect(uint8_t, uint8_t, QueueHandle_t)
 * @brief 释放电机保护
 * 
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_release_motor_protect(uint8_t addr, uint8_t interface, QueueHandle_t Queue);
/**
 * @fn uint8_t zdt_restart_motor(uint8_t, uint8_t, QueueHandle_t)
 * @brief 重启电机
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_restart_motor(uint8_t addr, uint8_t interface, QueueHandle_t Queue);
/**
 * @fn void Select_Can_Send(int8_t, _CANMSG*)
 * @brief 选择电机命令发送接口
 *
 * @param interface 电机接口编号
 * @param msg       消息内容
 */
uint8_t Select_Can_Send(int8_t interface, _CANMSG *msg);

#endif /* MOTOR_MOTOR_ZDT_H_ */
