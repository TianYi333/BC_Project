#include "pid_ctrl.h"
#include "main_logic.h"

PID_Handle_t g_pressure_pid;

void PID_UpdateParam(PID_Handle_t *hpid, _SYS_STATUS_1 *sys);

void PressureControlTask(void *argument)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    // 保存上一次下发的转速，用于防抖
    static float last_out_speed = 0.0f;
    // 转速变化阈值：小于该差值不下发Modbus，避免频繁通讯
    const float SPD_CHG_THRESHOLD = 5.0f; 
    for(;;)
    {
        // 快照拷贝，防止多任务竞争数据撕裂
        _SYS_STATUS_1 sys_tmp;
        taskENTER_CRITICAL();
        sys_tmp = main_sys_status_1;
        taskEXIT_CRITICAL();
        PID_UpdateParam(&g_pressure_pid, &sys_tmp);

        float output_speed = 0.0f;
        float ref_p        = sys_tmp.ref_pressure;
        float curr_p       = sys_tmp.adc_pressure;
        float full_max_spd = sys_tmp.max_motor_speed;
        float hold_min_spd = sys_tmp.min_motor_speed;

        // 仅加压/保压状态运行压力闭环
        if(sys_tmp.sys_start == SYS_STATE_PRESS || sys_tmp.sys_start == SYS_STATE_HOLD)
        {
            g_pressure_pid.set_val = ref_p;
            g_pressure_pid.fb_val  = curr_p;

            if(sys_tmp.sys_start == SYS_STATE_PRESS)
            {
                // 加压模式：允许0 ~ 最大电机转速
                output_speed = PID_Calc(&g_pressure_pid, 0.0f, full_max_spd);
            }
            else if(sys_tmp.sys_start == SYS_STATE_HOLD)
            {
                /*
                保压区间 [ref_p - PRESS_HYSTERESIS , ref_p]
                压力越靠近目标压力，最大允许转速线性降低，实现节能保压
                */
                float delta = ref_p - curr_p;
                float ratio = delta / PRESS_HYSTERESIS;
                float limit_speed = full_max_spd * ratio;

                // 最低不低于保压维持最小转速
                if(limit_speed < hold_min_spd)
                {
                    limit_speed = hold_min_spd;
                }
                output_speed = PID_Calc(&g_pressure_pid, 0.0f, limit_speed);
            }
        }
        else
        {
            // 自检/停机/泄压/参数设置：转速清零，复位PID消除历史误差
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
        }
        // ==========转速变化很小时，跳过Modbus下发，减轻485负载==========
        if(fabsf(output_speed - last_out_speed) > SPD_CHG_THRESHOLD)
        {
            // 下发目标转速至伺服
            Motor_SetTargetSpeed(output_speed);
            last_out_speed = output_speed;

            // 同步更新全局电机目标转速
            taskENTER_CRITICAL();
            main_sys_status_1.motor_status.target_speed = (uint32_t)output_speed;
            taskEXIT_CRITICAL();
        }
        // =========动态任务周期逻辑=========
        uint32_t delay_ms = 20;
        float pressure_err = fabsf(ref_p - curr_p);
        // 保压且压力偏差极小，进入稳态，拉长周期降低系统负载
        if(sys_tmp.sys_start == SYS_STATE_HOLD && pressure_err < (PRESS_HYSTERESIS * 0.3f))
        {
            delay_ms = 50;
        }
        
        vTaskDelay(pdMS_TO_TICKS(delay_ms)); // 由10ms → 20ms
    }
}

void PID_Init(PID_Handle_t *hpid, float kp, float ki, float kd)
{
    hpid->Kp = kp;
    hpid->Ki = ki;
    hpid->Kd = kd;
    PID_Reset(hpid);
}

void PID_Reset(PID_Handle_t *hpid)
{
    hpid->err = 0.0f;
    hpid->err_last = 0.0f;
    hpid->err_prev = 0.0f;
    hpid->output = 0.0f;
}

float PID_Calc(PID_Handle_t *hpid, float min_out, float max_out)
{
    hpid->err = hpid->set_val - hpid->fb_val;

    // 增量标准三项
    float P = hpid->Kp * (hpid->err - hpid->err_last);
    float I = hpid->Ki * hpid->err;
    float D = hpid->Kd * (hpid->err - 2.0f * hpid->err_last + hpid->err_prev);
    float delta = P + I + D;

    // 增量式抗积分饱和核心：输出饱和且增量同向，禁止叠加
    float temp_out = hpid->output + delta;
    if( (temp_out > max_out && delta > 0.0f) || (temp_out < min_out && delta < 0.0f) )
    {
        delta = 0.0f;
    }

    hpid->output += delta;

    // 兜底输出限幅
    if(hpid->output > max_out) hpid->output = max_out;
    if(hpid->output < min_out) hpid->output = min_out;

    // 更新误差缓存
    hpid->err_prev = hpid->err_last;
    hpid->err_last = hpid->err;

    return hpid->output;
}

// 从系统全局参数刷新PID三组系数
void PID_UpdateParam(PID_Handle_t *hpid, _SYS_STATUS_1 *sys)
{
    hpid->Kp = sys->pid_kp;
    hpid->Ki = sys->pid_ki;
    hpid->Kd = sys->pid_kd;
}

