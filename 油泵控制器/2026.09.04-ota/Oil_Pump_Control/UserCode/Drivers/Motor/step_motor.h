#ifndef __STEP_MOTOR_H
#define __STEP_MOTOR_H


#include "main.h"
#include <stdint.h>
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif


//==================== 硬件输入模式选择 二选一 ====================
#define STEP_DRV_COMMON_CATHODE     1   // 共阴极（默认，PU‑ DIR‑ MF‑接GND，MCU接+端）
//#define STEP_DRV_COMMON_ANODE       1   // 共阳极（PU+ DIR+ MF+接+5V，MCU接‑端）

#if defined(STEP_DRV_COMMON_CATHODE) && defined(STEP_DRV_COMMON_ANODE)
#error "不能同时定义 STEP_DRV_COMMON_CATHODE 和 STEP_DRV_COMMON_ANODE,请二选一"
#endif

#if defined(STEP_DRV_COMMON_CATHODE)
#define DIR_FORWARD_PIN_STATE     GPIO_PIN_SET
#define DIR_BACKWARD_PIN_STATE    GPIO_PIN_RESET
#define MF_RELEASE_PIN_STATE      GPIO_PIN_SET
#define MF_LOCK_PIN_STATE         GPIO_PIN_RESET
#elif defined(STEP_DRV_COMMON_ANODE)
#define DIR_FORWARD_PIN_STATE     GPIO_PIN_RESET
#define DIR_BACKWARD_PIN_STATE    GPIO_PIN_SET
#define MF_RELEASE_PIN_STATE      GPIO_PIN_RESET
#define MF_LOCK_PIN_STATE         GPIO_PIN_SET
#else
#error "必须选择 STEP_DRV_COMMON_CATHODE 或者 STEP_DRV_COMMON_ANODE"
#endif
//================================================================


#define DIR_GPIO_PORT       DO_OUTPUT_2_GPIO_Port
#define DIR_GPIO_PIN        DO_OUTPUT_2_Pin


#define MF_GPIO_PORT        DO_OUTPUT_1_GPIO_Port
#define MF_GPIO_PIN         DO_OUTPUT_1_Pin



/* 电机参数（驱动器拨码：5000细分）*/
#define PULSE_PER_REV       5000U        //每转脉冲数
#define MAX_RPM             550U         //5000细分驱动器最大转速
#define ACCEL_RPM_PER_S     100U         //加减速斜率 rpm/s
#define TASK_PERIOD_MS      5U          //任务周期，单位ms


/**
 * @brief 设置电机目标转速
 * @param rpm 范围 -MAX_RPM ~ +MAX_RPM；0 = 减速停止
 */
void step_motor_set_rpm(int16_t rpm);


/**
 * @brief 电机急停，立刻关闭脉冲输出
 */
void step_motor_emergency_stop(void);


/**
 * @brief 获取电机当前实际转速
 * @return 当前转速 rpm
 */
int16_t step_motor_get_curr_rpm(void);


/**
 * @brief 设置电机锁轴/释放脱机
 * @param enable true=释放脱机；false=锁轴(正常运行必须锁轴)
 */
void step_motor_set_release(bool enable);


/**
 * @brief 读取驱动器RDY就绪信号
 * @retval 1 驱动器就绪；0 故障/未就绪
 */
uint8_t step_motor_read_rdy(void);


/**
 * @brief 步进电机FreeRTOS任务=转速控制任务
 */
void step_speed_ctrl_task(void *arg);


#ifdef __cplusplus
}
#endif


#endif
