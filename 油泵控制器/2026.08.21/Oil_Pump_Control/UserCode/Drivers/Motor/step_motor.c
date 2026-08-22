#include "step_motor.h"
#include "FreeRTOS.h"
#include "task.h"
#include "tim.h"
#include <stdlib.h>
#include <string.h>


typedef struct
{
    int16_t target_rpm;         //目标转速，正数正转，负数反转，0停止
    int16_t curr_rpm;           //当前实际转速
    bool emergency_stop;        //急停标志
} StepMotorCtrl_T;

static StepMotorCtrl_T g_step_motor;

// /**
//  * @brief rpm换算脉冲频率Hz
//  */
// static uint32_t rpm_to_pulse_freq(int16_t rpm)
// {
//     if(rpm == 0)
//         return 0U;
//     uint32_t freq = ((uint32_t)abs(rpm) * PULSE_PER_REV) / 60U;
//     return freq;
// }

// /**
//  * @brief 设置TIM15 CH2 PWM脉冲输出
//  * TIM15时钟200MHz，PSC预分频=9 → 计数时钟 20MHz
//  * auto‑reload preload = Enable，等待当前周期结束更新ARR，防止畸形脉冲
//  */
// static void set_step_pwm_freq(uint32_t freq_hz)
// {
//     if(freq_hz == 0U)
//     {
//         HAL_TIM_PWM_Stop(&htim15, TIM_CHANNEL_2);
//         return;
//     }

//     const uint32_t tim_cnt_clk = 2000000U;
//     uint32_t arr = tim_cnt_clk / freq_hz;
//     if(arr < 2U) arr = 2U;

//     __HAL_TIM_SET_AUTORELOAD(&htim15, arr);
//     __HAL_TIM_SET_COMPARE(&htim15, TIM_CHANNEL_2, arr / 2U);

//     if(!(htim15.Instance->CR1 & TIM_CR1_CEN))
//     {
//         HAL_TIM_PWM_Start(&htim15, TIM_CHANNEL_2);
//     }
// }

/**
 * @brief 根据转速rpm直接设置TIM15 CH2步进脉冲输出
 * @param rpm 电机转速，正负代表转向，0停止输出
 * TIM15时钟240MHz，PSC预分频=119 → 计数时钟 2MHz
 * auto‑reload preload = Enable，等待当前周期结束更新ARR，防止畸形脉冲
 */
static void set_step_pwm_by_rpm(int16_t rpm)
{
    if(rpm == 0)
    {
        HAL_TIM_PWM_Stop(&htim15, TIM_CHANNEL_2);
        return;
    }

    int16_t rpm_abs = (rpm >= 0) ? rpm : -rpm;
    uint32_t freq_hz = ((uint32_t)rpm_abs * PULSE_PER_REV) / 60U;

    const uint32_t tim_cnt_clk = 2000000U;  // ✅ 计数时钟：240M/(119+1)
    uint32_t arr = tim_cnt_clk / freq_hz;

    // TIM15是16位，ARR范围：2 ~ 65535
    if(arr < 2U)  arr = 2U;
    if(arr > 65535U) arr = 65535U;

    __HAL_TIM_SET_AUTORELOAD(&htim15, arr);
    __HAL_TIM_SET_COMPARE(&htim15, TIM_CHANNEL_2, arr / 2U);

    if(!(htim15.Instance->CR1 & TIM_CR1_CEN))
    {
        HAL_TIM_PWM_Start(&htim15, TIM_CHANNEL_2);
    }
}


uint8_t step_motor_read_rdy(void)//读取步进电机就绪状态
{
    if(HAL_GPIO_ReadPin(RDY_GPIO_PORT, RDY_GPIO_PIN) == GPIO_PIN_SET)
    {
        return 1U;
    }
    return 0U;
}

void step_motor_set_release(bool enable)//设置步进电机释放状态
{
    if(enable)
    {
        HAL_GPIO_WritePin(MF_GPIO_PORT, MF_GPIO_PIN, MF_RELEASE_PIN_STATE);
    }
    else
    {
        HAL_GPIO_WritePin(MF_GPIO_PORT, MF_GPIO_PIN, MF_LOCK_PIN_STATE);
    }
}

void step_speed_ctrl_task(void *arg)
{
    (void)arg;
    memset(&g_step_motor, 0, sizeof(g_step_motor));

    /* 上电默认锁轴 */
    step_motor_set_release(false);

    static int16_t last_curr_rpm = 0; // 保存上一次生效转速
    static int16_t last_target_rpm = 0; // 保存上一次目标转速

    for(;;)
    {
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
        //float delta_rpm_max = ACCEL_RPM_PER_S * TASK_PERIOD_MS / 1000.0f;

        if(g_step_motor.emergency_stop)
        {
            g_step_motor.curr_rpm = 0;
            g_step_motor.target_rpm = 0;
            g_step_motor.emergency_stop = false;
            set_step_pwm_by_rpm(0);
            last_curr_rpm  = 0;
            last_target_rpm = 0;
            continue;
        }

        int16_t target = g_step_motor.target_rpm;
        if(target > (int16_t)MAX_RPM)  target = MAX_RPM;
        if(target < -(int16_t)MAX_RPM) target = -(int16_t)MAX_RPM;

        // 方向：仅target改变时才改写DIR引脚
        if(target != last_target_rpm)
        {
            if(target > 0)
            {
                HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_GPIO_PIN, DIR_FORWARD_PIN_STATE);
            }
            else if(target < 0)
            {
                HAL_GPIO_WritePin(DIR_GPIO_PORT, DIR_GPIO_PIN, DIR_BACKWARD_PIN_STATE);
            }
            last_target_rpm = target;
        }

        /* 梯形加减速 */
        // if(g_step_motor.curr_rpm < target)
        // {
        //     g_step_motor.curr_rpm += (int16_t)(delta_rpm_max + 0.5f);
        //     if(g_step_motor.curr_rpm > target)
        //         g_step_motor.curr_rpm = target;
        // }
        // else if(g_step_motor.curr_rpm > target)
        // {
        //     g_step_motor.curr_rpm -= (int16_t)(delta_rpm_max + 0.5f);
        //     if(g_step_motor.curr_rpm < target)
        //         g_step_motor.curr_rpm = target;
        // }
        if(g_step_motor.curr_rpm < target)
        {
            g_step_motor.curr_rpm += 1;
            if(g_step_motor.curr_rpm > target)
                g_step_motor.curr_rpm = target;
        }
        else if(g_step_motor.curr_rpm > target)
        {
            g_step_motor.curr_rpm -= 1;
            if(g_step_motor.curr_rpm < target)
                g_step_motor.curr_rpm = target;
        }
        // ==========关键改动：转速变化才更新PWM==========
        if(g_step_motor.curr_rpm != last_curr_rpm)
        {
            set_step_pwm_by_rpm(g_step_motor.curr_rpm);
            last_curr_rpm = g_step_motor.curr_rpm;
        }
    }
}

void step_motor_set_rpm(int16_t rpm)
{
    g_step_motor.target_rpm = rpm;
}

void step_motor_emergency_stop(void)
{
    g_step_motor.emergency_stop = true;
}

int16_t step_motor_get_curr_rpm(void)
{
    return g_step_motor.curr_rpm;
}




