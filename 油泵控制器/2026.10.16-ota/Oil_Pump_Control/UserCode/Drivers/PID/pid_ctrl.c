#include "pid_ctrl.h"
#include "main_logic.h"
#include "step_motor.h"

PID_Handle_t g_pressure_pid;

void PID_UpdateParam(PID_Handle_t *hpid, SYS_CONFIG_T *cfg);



#if defined(MOTOR_DRIVER_RS485_SERVO)
/**
 * @brief 压力闭环任务：485伺服电机版本
 */
void PressureControlTask(void *argument)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    // 保存上一次下发的转速，用于防抖
    static float last_out_speed = 0.0f;
    // target_speed快照写入缓存，减少锁调用
    static float last_write_target = -1.0f;
    // 转速变化阈值：小于该差值不下发Modbus，避免频繁通讯
    const float SPD_CHG_THRESHOLD = 1.0f;
    // target_speed快照更新阈值，很小，保证寄存器读数跟随
    const float TARGET_SPD_WRITE_THRESHOLD = 0.2f;
    // 压力预警区间比例：距离目标压力小于该范围，启动预减速防超调
    const float PRESS_PRE_CONTROL_RATIO = 0.25f; 

    for(;;)
    {
        // 快照拷贝，防止多任务竞争数据撕裂
        _SYS_STATUS sys_tmp;
        uint8_t read_ok = SysStatus_ReadSnapshot(&sys_tmp);
        // 如果读取系统状态失败，跳过本次闭环计算
        if(!read_ok)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 伺服驱动器实际模式校验
        if(sys_tmp.motor_status.real_ctrl_mode != MOTOR_CTRL_MODE_VEL)
        {
            PID_Reset(&g_pressure_pid);
            last_out_speed = 0.0f;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 加锁读取配置快照，防止并发写入撕裂
        SYS_CONFIG_T cfg_tmp;
        SysConfig_ReadSnapshot(&cfg_tmp);

        PID_UpdateParam(&g_pressure_pid, &cfg_tmp);

        float output_speed = 0.0f;//闭环计算输出转速，单位：rpm
        float curr_p       = sys_tmp.adc_pressure;//当前压力值
        float ref_p        = cfg_tmp.ref_pressure;//目标压力值
        float full_max_spd = cfg_tmp.max_motor_speed;//最大转速
        float hold_min_spd = cfg_tmp.min_motor_speed;//保压最小转速
        float press_hyst   = cfg_tmp.press_hysteresis;//压力回差
        float max_press_lmt= cfg_tmp.max_pressure;//最大压力限制

        // =====压力到达最高限制，直接禁止输出=====
        if(curr_p >= max_press_lmt)
        {
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
        }
        // 故障状态禁止闭环输出
        else if(sys_tmp.motor_status.state == MOTOR_STATE_FAULT || sys_tmp.sys_fault_bit != 0U)
        {
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
        }
        // =========设置模式：关闭PID，读取手动转速，允许外部调速=========
        else if(sys_tmp.sys_start == SYS_STATE_SETTING)
        {
            PID_Reset(&g_pressure_pid); // 停止PID积分运算，彻底断开闭环
            output_speed = sys_tmp.manual_set_speed; // 取用外部设置的手动转速
        }
        // 加压/保压模式：正常PID闭环运算
        else if(sys_tmp.sys_start == SYS_STATE_PRESS || sys_tmp.sys_start == SYS_STATE_HOLD)
        {
            g_pressure_pid.set_val = ref_p;
            g_pressure_pid.fb_val  = curr_p;

            if(sys_tmp.sys_start == SYS_STATE_PRESS)
            {
                float upper_limit = full_max_spd;
                // =========预控防超压逻辑（加压阶段生效）=========
                float err = ref_p - curr_p;
                float pre_control_zone = ref_p * PRESS_PRE_CONTROL_RATIO;

                // 压力距离目标很近，进入预警区间，线性降低最大转速
                if(err > 0 && err <= pre_control_zone)
                {
                    float scale = err / pre_control_zone;
                    upper_limit = full_max_spd * scale;
                }
                // =================================================
                output_speed = PID_Calc(&g_pressure_pid, 0.0f, upper_limit);
            }
            else if(sys_tmp.sys_start == SYS_STATE_HOLD)
            {
                /*
                保压区间 [ref_p - press_hyst , ref_p]
                压力越靠近目标压力，最大允许转速线性降低，实现节能保压
                */
                float delta = ref_p - curr_p;
                if(delta < 0.0f)
                {
                    delta = 0.0f;
                }
                float limit_speed;
                // 防止除0保护
                if(press_hyst < 0.01f)
                {
                    limit_speed = hold_min_spd;
                }
                else
                {
                    float ratio = delta / press_hyst;
                    limit_speed = full_max_spd * ratio;
                }

                // 最低不低于保压维持最小转速
                if(limit_speed < hold_min_spd)
                {
                    limit_speed = hold_min_spd;
                }
                output_speed = PID_Calc(&g_pressure_pid, 0.0f, limit_speed);
                // 保压模式下：已经输出最低转速，但压力还没上来，强制锁死最低转速输出
                // 只有压力 >= max_press_lmt 才会在上层条件把output_speed清零
                if(output_speed <= hold_min_spd)
                {
                    output_speed = hold_min_spd;
                }
            }
        }
        else
        {
            // 停机/泄压/待机：关闭输出，复位PID
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
            last_out_speed = 0.0f;
        }

        // 安全钳位，禁止负转速
        if(output_speed < 0.0f)
        {
            output_speed = 0.0f;
        }
        // 全局限速兜底，手动/自动转速都不能超上限
        if(output_speed > full_max_spd)
        {
            output_speed = full_max_spd;
        }

        // 转速防抖，小幅变化不发送Modbus
        if(fabsf(output_speed - last_out_speed) > SPD_CHG_THRESHOLD)
        {
            MotorCtrl_SubmitSetSpeed(output_speed);
            last_out_speed = output_speed;
        }

        // 快照更新逻辑区分状态：
        // 1. 加压/保压：更新自动闭环target_speed
        // 2. 设置模式：不覆盖motor_status.target_speed，保护闭环参数不被手动转速冲掉
        if(sys_tmp.sys_start != SYS_STATE_SETTING)
        {
            if(fabsf(output_speed - last_write_target) > TARGET_SPD_WRITE_THRESHOLD)
            {
                _SYS_STATUS write_tmp;
                if(SysStatus_ReadSnapshot(&write_tmp))
                {
                    write_tmp.motor_status.target_speed = (uint32_t)output_speed;
                    SysStatus_WriteSnapshot(&write_tmp);
                    last_write_target = output_speed;
                }
            }
        }

        // 动态任务周期
        uint32_t delay_ms = 20;
        float pressure_err = fabsf(ref_p - curr_p);
        if(sys_tmp.sys_start == SYS_STATE_HOLD && pressure_err < (press_hyst * 0.3f))
        {
            delay_ms = 50;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

#elif defined(MOTOR_DRIVER_PULSE_STEPPER)
/**
 * @brief 压力闭环任务：脉冲步进电机版本
 * 改动点：替换下发接口，移除485 target_speed快照逻辑
 */
void PressureControlTask(void *argument)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    // 保存【真正下发给电机】的整数转速，用于防抖
    static int16_t last_set_rpm = 0;
    // 转速变化阈值：小于该差值不下发，步进最小1rpm
    const int16_t SPD_CHG_THRESHOLD = 1;
    // 压力预警区间比例：距离目标压力小于该范围，启动预减速防超调
    const float PRESS_PRE_CONTROL_RATIO = 0.25f;

    // 缓存已经写入快照的actual_speed，转速不变就跳过写快照
    static float last_refresh_actual_spd = -9999.0f;

    for(;;)
    {
        // 快照拷贝，防止多任务竞争数据撕裂
        _SYS_STATUS sys_tmp;
        uint8_t read_ok = SysStatus_ReadSnapshot(&sys_tmp);
        if(!read_ok)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        SYS_CONFIG_T cfg_tmp;
        SysConfig_ReadSnapshot(&cfg_tmp);
        PID_UpdateParam(&g_pressure_pid, &cfg_tmp);

        float output_speed = 0.0f;
        float curr_p     = sys_tmp.adc_pressure;//当前压力，单位：MPa
        float ref_p      = cfg_tmp.ref_pressure;//目标压力，单位：MPa
        float full_max_spd = cfg_tmp.max_motor_speed;//最大转速，单位：rpm
        float hold_min_spd = cfg_tmp.min_motor_speed;//保压模式下，最小转速，单位：rpm
        float press_hyst   = cfg_tmp.press_hysteresis;//压力死区，单位：MPa
        float max_press_lmt= cfg_tmp.max_pressure;//最大压力限制，单位：MPa

        if(curr_p >= max_press_lmt)
        {
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
        }
        else if(sys_tmp.sys_fault_bit != 0U)
        {
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
        }
        else if(sys_tmp.sys_start == SYS_STATE_SETTING)
        {
            PID_Reset(&g_pressure_pid);
            output_speed = sys_tmp.manual_set_speed;
            last_set_rpm = 0;
        }
        else if(sys_tmp.sys_start == SYS_STATE_PRESS || sys_tmp.sys_start == SYS_STATE_HOLD)
        {
            g_pressure_pid.set_val = ref_p;
            g_pressure_pid.fb_val  = curr_p;

            if(sys_tmp.sys_start == SYS_STATE_PRESS)
            {
                float upper_limit = full_max_spd;
                float err = ref_p - curr_p;
                float pre_control_zone = ref_p * PRESS_PRE_CONTROL_RATIO;
                if(err > 0 && err <= pre_control_zone)
                {
                    float scale = err / pre_control_zone;
                    upper_limit = full_max_spd * scale;
                }
                output_speed = PID_Calc(&g_pressure_pid, 0.0f, upper_limit);
            }
            else if(sys_tmp.sys_start == SYS_STATE_HOLD)
            {
                float delta = ref_p - curr_p;
                if(delta < 0.0f) delta = 0.0f;
                float limit_speed;
                if(press_hyst < 0.01f)
                {
                    limit_speed = hold_min_spd;
                }
                else
                {
                    float ratio = delta / press_hyst;
                    limit_speed = full_max_spd * ratio;
                }
                if(limit_speed < hold_min_spd)
                {
                    limit_speed = hold_min_spd;
                }
                output_speed = PID_Calc(&g_pressure_pid, 0.0f, limit_speed);
                if(output_speed <= hold_min_spd)
                {
                    output_speed = hold_min_spd;
                }
            }
        }
        else
        {
            output_speed = 0.0f;
            PID_Reset(&g_pressure_pid);
            last_set_rpm = 0;
        }

        if(output_speed < 0.0f) output_speed = 0.0f;
        if(output_speed > full_max_spd) output_speed = full_max_spd;

        // ============关键修改：PID浮点输出四舍五入得到整数rpm============
        int16_t target_rpm = (int16_t)(output_speed + 0.5f);
        // 硬件最小步进1rpm，和最大限速钳位
        if(target_rpm < 0) target_rpm = 0;
        if(target_rpm > (int16_t)full_max_spd) target_rpm = (int16_t)full_max_spd;

        // 和上一次真正下发的整数转速做比较，手动求绝对值，无库依赖
        int16_t delta_rpm = target_rpm - last_set_rpm;
        if(delta_rpm < 0)
        {
            delta_rpm = (int16_t)-delta_rpm;
        }
        if( delta_rpm >= SPD_CHG_THRESHOLD )
        {
            step_motor_set_rpm(target_rpm);
            last_set_rpm = target_rpm;
        }

        // 动态任务周期
        uint32_t delay_ms = 20;
        float pressure_err = fabsf(ref_p - curr_p);
        if(sys_tmp.sys_start == SYS_STATE_HOLD && pressure_err < (press_hyst * 0.3f))
        {
            delay_ms = 50;
        }

        // 刷新实际转速到全局电机状态，因电机速度控制任务周期延时仅5ms，全局变量写入与读取获取锁超时时间20ms，所以放在这里比较合适
        float curr_act_spd = (float)step_motor_get_curr_rpm();
        if(curr_act_spd != last_refresh_actual_spd)
        {
            _SYS_STATUS status_tmp;
            if(SysStatus_ReadSnapshot(&status_tmp))
            {
                status_tmp.motor_status.actual_speed = curr_act_spd;
                SysStatus_WriteSnapshot(&status_tmp);
            }
            last_refresh_actual_spd = curr_act_spd;
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}



#endif

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
void PID_UpdateParam(PID_Handle_t *hpid, SYS_CONFIG_T *cfg)
{
    hpid->Kp = cfg->pid_kp;
    hpid->Ki = cfg->pid_ki;
    hpid->Kd = cfg->pid_kd;
}

