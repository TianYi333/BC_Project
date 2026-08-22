#ifndef __PID_CTRL_H
#define __PID_CTRL_H

#include "main.h"
#include "modbus_master.h"

struct _SYS_STATUS;
typedef struct
{
    float Kp;
    float Ki;
    float Kd;

    float set_val;   // 目标压力
    float fb_val;    // 反馈压力

    float err;
    float err_last;
    float err_prev;

    float output;
} PID_Handle_t;

extern PID_Handle_t g_pressure_pid;

void PressureControlTask(void *argument);

// 增量PID计算
float PID_Calc(PID_Handle_t *hpid, float min_out, float max_out);
// 复位偏差，消除积分饱和
void PID_Reset(PID_Handle_t *hpid);
// 初始化PID参数
void PID_Init(PID_Handle_t *hpid, float kp, float ki, float kd);

#endif

