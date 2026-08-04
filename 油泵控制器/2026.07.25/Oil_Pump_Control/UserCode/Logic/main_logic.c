#include "main_logic.h"


// _SYS_STATUS_1 main_sys_status_1;
// 系统全局状态实例
_SYS_STATUS_1 main_sys_status_1 = {
    .sys_start = SYS_STATE_SELFTEST,
    .sys_warning = 0,
    .valve_state = 0,

    .adc_pressure = 0.0f,
    .adc_oil_temp = 0.0f,
    .liquid_level_pct = 0.0f,
    .ref_pressure = 10.0f,

    .max_pressure = 20.0f,
    .max_motor_speed = 3000.0f,
    .min_motor_speed = 200.0f,
    .max_oil_temp = 85.0f,
    .min_liquid_level = 20.0f,

    .sensor_status = SENSOR_STATUS_FAULT,
    .sensor_err_bit = 0,

    .motor_status = {
        .comm_lost = 0U,
        .state = MOTOR_STATE_STOP,
        .err_code = 0,
        .status_word = 0,
        .real_ctrl_mode = MOTOR_CTRL_MODE_VEL,
        .set_ctrl_mode = MOTOR_CTRL_MODE_VEL,
        .target_speed = 0,
        .target_acc = 100,
        .target_dec = 100,
        .actual_speed = 0
    }
};

// 压力控制回差，避免频繁切换状态
const float PRESS_HYSTERESIS = 0.3f;
uint32_t stop_enter_tick = 0U;       // 进入停机状态时刻tick

void MainLogicTask(void *argument)
{
    uint8_t last_sys_state = SYS_STATE_SELFTEST;

    // 标记：是否已经设置伺服为速度模式（仅自检通过执行一次）
    uint8_t motor_mode_init_flag = 0;

    // 先延时3秒，等待电机上电自检完成
    vTaskDelay(pdMS_TO_TICKS(3000));
    for(;;)
    {
        // 临界区拷贝系统快照，防止多任务数据撕裂
        _SYS_STATUS_1 sys_tmp;
        taskENTER_CRITICAL();
        sys_tmp = main_sys_status_1;
        taskEXIT_CRITICAL();

        uint8_t motor_fault = 0;
        uint8_t sensor_fault = 0;

        // ============电机故障判定：故障码非0  OR 状态字Bit3置1（驱动器错误）============
        if((sys_tmp.motor_status.err_code != 0U) ||
           ((sys_tmp.motor_status.status_word & MOTOR_STATUS_FAULT_BIT) != 0U))
        {
            motor_fault = 1;
        }
        // ============直接通过故障位图判断所有传感器故障（压力、油温、液位）============
        if(sys_tmp.sensor_err_bit != 0U)
        {
            sensor_fault = 1;
        }
        uint8_t motor_comm_lost_flag = sys_tmp.motor_status.comm_lost;
        uint8_t total_fault = motor_fault || sensor_fault || motor_comm_lost_flag;

        //============上电自检分支 ============
        if(sys_tmp.sys_start == SYS_STATE_SELFTEST)
        {
            uint8_t motor_comm_fault_self = 0;
            if(last_sys_state != SYS_STATE_SELFTEST)
            {
                LOG_Debug("Power On Self Test Start");
                show_oled_msg(OLED_MSG_UPDATE, "Self Test Start...");
                taskENTER_CRITICAL();
                main_sys_status_1.valve_state = 0; // 自检关闭泄压阀
                main_sys_status_1.motor_status.state = MOTOR_STATE_WAIT;
                main_sys_status_1.motor_status.target_speed = 0;// 自检时速度模式下，目标速度设为0
                taskEXIT_CRITICAL();
                Motor_SetTargetSpeed(0.0f);
                PID_Reset(&g_pressure_pid);
            }
            // ============自检阶段：额外检测电机Modbus物理通讯链路============
            uint16_t test_err;
            int comm_ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 1001, 1, &test_err);
            if(comm_ret != 0)
            {
                motor_comm_fault_self = 1;
            }
            // ============上电自检主动读取液位传感器（首次上电获取液位）============
            uint16_t liquid_raw_init = LiquidSensor_ReadLevel();
            if(liquid_comm_err_cnt == 0U)
            {
                taskENTER_CRITICAL();
                // 根据你的传感器量程完成 raw → %换算
                main_sys_status_1.liquid_level_pct = (float)liquid_raw_init / 4000.0f * 100.0f;
                taskEXIT_CRITICAL();
            }
            // ============重新拷贝最新系统快照，读取更新后的故障位图============
            _SYS_STATUS_1 sys_tmp_new;
            taskENTER_CRITICAL();
            sys_tmp_new = main_sys_status_1;
            taskEXIT_CRITICAL();

            // 使用最新快照统一判定故障（和运行期逻辑一致）
            uint8_t self_motor_fault = 0;
            uint8_t self_sensor_fault = 0;

            if((sys_tmp_new.motor_status.err_code != 0U) ||
            ((sys_tmp_new.motor_status.status_word & MOTOR_STATUS_FAULT_BIT) != 0U))
            {
                self_motor_fault = 1;
            }
            if(sys_tmp_new.sensor_err_bit != 0U)
            {
                self_sensor_fault = 1;
            }

            // 统一故障汇总：电机链路故障 || 电机驱动器故障 || 任意传感器故障
            uint8_t self_test_fail = motor_comm_fault_self || self_motor_fault || self_sensor_fault;
            
            // ============存在故障：持续循环自检，不进入运行流程============
            if(self_test_fail)
            {
                if(motor_comm_fault_self)
                {
                    LOG_Debug("SelfTest Fail: Motor Modbus Comm Error");
                    show_oled_msg(OLED_MSG_ERROR, "Self-test: Motor comm fail");
                }
                if(self_sensor_fault)
                {
                    if(sys_tmp_new.sensor_err_bit & SENSOR_ERR_LIQUID)//液位传感器故障
                    {
                        LOG_Debug("SelfTest Fail: Liquid Sensor Comm Error");
                        show_oled_msg(OLED_MSG_ERROR, "Self-test: Liquid sensor fail");
                    }
                    else if(sys_tmp_new.sensor_err_bit & SENSOR_ERR_PRESSURE)//压力传感器故障
                    {
                        LOG_Debug("SelfTest Fail: Pressure Sensor Fault");
                        show_oled_msg(OLED_MSG_ERROR, "Self-test: Pressure sensor fail");
                    }
                    else if(sys_tmp_new.sensor_err_bit & SENSOR_ERR_OIL_TEMP)//油温传感器故障
                    {
                        LOG_Debug("SelfTest Fail: Oil Temp Sensor Fault");
                        show_oled_msg(OLED_MSG_ERROR, "Self-test: Oil temp sensor fail");
                    }
                    else
                    {
                        LOG_Debug("SelfTest Fail: Unknown Sensor Abnormal");
                        show_oled_msg(OLED_MSG_ERROR, "Self-test: Sensor abnormal");
                    }
                }
                if(self_motor_fault)
                {
                    LOG_Debug("SelfTest Fail: Motor Fault");
                    show_oled_msg(OLED_MSG_ERROR, "Self-test: Motor fault");
                    taskENTER_CRITICAL();
                    main_sys_status_1.motor_status.state = MOTOR_STATE_FAULT;
                    taskEXIT_CRITICAL();
                }
                last_sys_state = SYS_STATE_SELFTEST;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            // ============自检全部正常，完成自检============
            LOG_Debug("SelfTest Pass, Auto Switch Running State");

            // ============自检通过，一次性设置伺服为速度模式============
            if(motor_mode_init_flag == 0)
            {
                int ret = Motor_SetRunMode((uint16_t)MOTOR_CTRL_MODE_VEL);// 设置伺服为速度模式
                if(ret == 0)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.motor_status.set_ctrl_mode = MOTOR_CTRL_MODE_VEL;
                    taskEXIT_CRITICAL();
                    motor_mode_init_flag = 1;
                    LOG_Debug("Motor set to velocity mode success");
                }
                else
                {
                    LOG_Debug("Set velocity mode failed!");
                    last_sys_state = SYS_STATE_SELFTEST;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
            }

            // 根据当前压力自动切入对应运行状态
            taskENTER_CRITICAL();
            float curr_p = sys_tmp.adc_pressure;// 当前压力值
            float ref_p = sys_tmp.ref_pressure;// 参考压力值
            float max_p = sys_tmp.max_pressure;// 最大压力值 

            if(curr_p >= max_p)//压力超上限
            {
                main_sys_status_1.sys_start = SYS_STATE_RELEASE;
            }
            else if(curr_p >= ref_p - PRESS_HYSTERESIS)//压力足够，直接保压
            {
                main_sys_status_1.sys_start = SYS_STATE_HOLD;
            }
            else if(curr_p > 0.2f) //管道有压力，但低于保压阈值 → 上电后进入加压
            {
                main_sys_status_1.sys_start = SYS_STATE_PRESS;
            }
            else//管道接近无压，待机停机
            {
                main_sys_status_1.sys_start = SYS_STATE_STOP;
            }
            taskEXIT_CRITICAL();

            last_sys_state = main_sys_status_1.sys_start;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ============ 运行全程故障联锁（最高优先级）============
        if(total_fault)// 存在故障：电机故障或传感器故障或通讯故障
        {
            // ============存在故障，强制停机============
            if(sys_tmp.sys_start != SYS_STATE_STOP)
            {
                taskENTER_CRITICAL();
                main_sys_status_1.sys_start = SYS_STATE_STOP;
                main_sys_status_1.valve_state = 0;// 强制关闭泄压阀
                main_sys_status_1.motor_status.target_speed = 0;// 强制设置伺服速度为0，停止运行
                main_sys_status_1.motor_status.state = MOTOR_STATE_FAULT;// 强制设置伺服状态为故障
                taskEXIT_CRITICAL();
                Motor_SetTargetSpeed(0.0f);// 强制设置伺服速度为0，停止运行
                PID_Reset(&g_pressure_pid);
                LOG_Debug("System Fault, Force STOP");
            }

            // 区分故障类型，OLED持续显示对应错误
            if(motor_comm_lost_flag == 1U)//电机通讯故障
            {
                show_oled_msg(OLED_MSG_ERROR, "Motor comm lost!");
                LOG_Debug("Fault: Motor Modbus communication lost");
            }
            else if(motor_fault)//电机故障
            {
                show_oled_msg(OLED_MSG_ERROR, "Motor fault, stop running");
                LOG_Debug("Fault: Servo drive fault");
            }
            else if(sensor_fault)//传感器故障
            {
                uint8_t has_print = 0;
                if(sys_tmp.sensor_err_bit & SENSOR_ERR_PRESSURE)//压力传感器故障
                {
                    show_oled_msg(OLED_MSG_ERROR, "Fault: Pressure sensor");
                    LOG_Debug("Fault: Pressure sensor abnormal");
                    has_print = 1;
                }
                if(sys_tmp.sensor_err_bit & SENSOR_ERR_OIL_TEMP)//油温传感器故障
                {
                    show_oled_msg(OLED_MSG_ERROR, "Fault: Oil temp sensor");
                    LOG_Debug("Fault: Oil temperature sensor abnormal");
                    has_print = 1;
                }
                if(sys_tmp.sensor_err_bit & SENSOR_ERR_LIQUID)//液位传感器故障
                {
                    show_oled_msg(OLED_MSG_ERROR, "Fault: Liquid level sensor");
                    LOG_Debug("Fault: Liquid level 485 sensor abnormal");
                    has_print = 1;
                }
                if(has_print == 0)// 无故障
                {
                    show_oled_msg(OLED_MSG_ERROR, "Sensor abnormal, stop running");
                    LOG_Debug("Fault: Unknown sensor error");
                }
            }
            last_sys_state = SYS_STATE_STOP;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ============ 正常运行状态，实时根据压力自动切换============
        float curr_p = sys_tmp.adc_pressure;
        float ref_p = sys_tmp.ref_pressure;
        float max_p = sys_tmp.max_pressure;

        switch(sys_tmp.sys_start)
        {
            case SYS_STATE_STOP:
            {
                if(last_sys_state != SYS_STATE_STOP)
                {
                    LOG_Debug("Enter STOP Mode");
                    // 记录进入停机时刻系统tick
                    stop_enter_tick = xTaskGetTickCount();

                    taskENTER_CRITICAL();
                    main_sys_status_1.valve_state = 0;
                    main_sys_status_1.motor_status.target_speed = 0;
                    if(main_sys_status_1.motor_status.state != MOTOR_STATE_FAULT)
                    {
                        main_sys_status_1.motor_status.state = MOTOR_STATE_STOP;
                    }
                    taskEXIT_CRITICAL();

                    Motor_SetTargetSpeed(0.0f);
                    PID_Reset(&g_pressure_pid);
                    show_oled_msg(OLED_MSG_UPDATE, "System stop, standby");
                }

                // ============ 核心改动：判断停机稳定延时是否满足============
                uint32_t now_tick = xTaskGetTickCount();
                uint32_t elapsed_tick = now_tick - stop_enter_tick;
                // 未达到稳定等待时间 → 直接跳出，不执行压力判断
                if(elapsed_tick < pdMS_TO_TICKS(STOP_STABLE_DELAY_MS))
                {
                    break;
                }

                // ============ 停机稳定延时满足，压力稳定，再执行压力逻辑============
                // ============ 停机状态下，压力低于目标，自动进入加压============
                if(curr_p < ref_p - PRESS_HYSTERESIS && curr_p > 0.2f)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.sys_start = SYS_STATE_PRESS;
                    taskEXIT_CRITICAL();
                }
                // 停机压力超上限，自动泄压
                if(curr_p >= max_p)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.sys_start = SYS_STATE_RELEASE;
                    taskEXIT_CRITICAL();
                }
                break;
            }
            case SYS_STATE_PRESS:
            {
                if(last_sys_state != SYS_STATE_PRESS)
                {
                    LOG_Debug("Enter PRESS Mode");
                    PID_Reset(&g_pressure_pid);
                    taskENTER_CRITICAL();
                    main_sys_status_1.motor_status.state = MOTOR_STATE_RUN;// 强制设置伺服状态为运行中
                    taskEXIT_CRITICAL();
                    show_oled_msg(OLED_MSG_UPDATE, "Pressure boosting");
                }

                // ============ 达到目标压力，切保压============
                if(curr_p >= ref_p)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.sys_start = SYS_STATE_HOLD;
                    taskEXIT_CRITICAL();
                }
                // ============ 超过最高压力，强制停机============
                if(curr_p >= max_p)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.sys_start = SYS_STATE_STOP;
                    taskEXIT_CRITICAL();
                }
                break;
            }

            case SYS_STATE_HOLD:
            {
                if(last_sys_state != SYS_STATE_HOLD)
                {
                    LOG_Debug("Enter HOLD Mode");
                    PID_Reset(&g_pressure_pid);
                    taskENTER_CRITICAL();
                    main_sys_status_1.motor_status.state = MOTOR_STATE_RUN;
                    taskEXIT_CRITICAL();
                    show_oled_msg(OLED_MSG_UPDATE, "Pressure holding");
                }

                // ============ 压力回落低于回差，重新加压============
                if(curr_p < ref_p - PRESS_HYSTERESIS)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.sys_start = SYS_STATE_PRESS;
                    taskEXIT_CRITICAL();
                }
                // ============ 超过最高压力，强制停机============
                if(curr_p >= max_p)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.sys_start = SYS_STATE_STOP;
                    taskEXIT_CRITICAL();
                }
                break;
            }

            case SYS_STATE_RELEASE:
            {
                if(last_sys_state != SYS_STATE_RELEASE)
                {
                    LOG_Debug("Enter RELEASE Mode, Open Valve");
                    taskENTER_CRITICAL();
                    main_sys_status_1.valve_state = 1;
                    main_sys_status_1.motor_status.target_speed = 0;
                    if(main_sys_status_1.motor_status.state != MOTOR_STATE_FAULT)
                    {
                        main_sys_status_1.motor_status.state = MOTOR_STATE_STOP;
                    }
                    taskEXIT_CRITICAL();
                    Motor_SetTargetSpeed(0.0f);
                    PID_Reset(&g_pressure_pid);
                    // 显示泄压状态
                    show_oled_msg(OLED_MSG_UPDATE, "Pressure releasing");
                }

                // ============ 压力释放完毕，关闭阀门切停机============
                if(curr_p <= 0.2f)
                {
                    taskENTER_CRITICAL();
                    main_sys_status_1.valve_state = 0;
                    main_sys_status_1.sys_start = SYS_STATE_STOP;
                    taskEXIT_CRITICAL();
                }
                break;
            }

            case SYS_STATE_SETTING:
            {
                if(last_sys_state != SYS_STATE_SETTING)
                {
                    LOG_Debug("Enter SETTING Mode");
                    taskENTER_CRITICAL();
                    main_sys_status_1.motor_status.target_speed = 0;
                    if(main_sys_status_1.motor_status.state != MOTOR_STATE_FAULT)
                    {
                        main_sys_status_1.motor_status.state = MOTOR_STATE_STOP;
                    }
                    taskEXIT_CRITICAL();

                    Motor_SetTargetSpeed(0.0f);
                    PID_Reset(&g_pressure_pid);
                }
                // ============ 设置模式不自动切换压力流程，仅手动退出回到停机============
                break;
            }

            default:
            {
                taskENTER_CRITICAL();
                main_sys_status_1.sys_start = SYS_STATE_STOP;
                main_sys_status_1.motor_status.target_speed = 0;
                if(main_sys_status_1.motor_status.state != MOTOR_STATE_FAULT)
                {
                    main_sys_status_1.motor_status.state = MOTOR_STATE_STOP;
                }
                taskEXIT_CRITICAL();

                Motor_SetTargetSpeed(0.0f);
                PID_Reset(&g_pressure_pid);
                break;
            }
        }

        last_sys_state = main_sys_status_1.sys_start;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}














