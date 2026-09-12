#include "main_logic.h"


// 读取全局系统状态快照
// return: 1=成功拿到锁、拷贝有效；0=锁获取失败，降级裸拷贝（兜底）
uint8_t SysStatus_ReadSnapshot(_SYS_STATUS *dst);

// 修改全局系统状态
// return: 1=成功；0=锁获取失败，无法安全写入
uint8_t SysStatus_WriteSnapshot(_SYS_STATUS *src);

// _SYS_STATUS main_sys_status;
// 系统全局状态实例
_SYS_STATUS main_sys_status;
SYS_CONFIG_T sys_cfg;
FAULT_REPORT_T g_fault_report = {0};
AlarmRecord g_alarm_records[ALARM_RECORD_MAX_CNT] = {0};
uint16_t g_alarm_record_cnt = 0U;

// =========系统状态初始化函数=========
void SysStatus_Init(void)
{
    main_sys_status.sys_start = SYS_STATE_STOP;
    main_sys_status.sys_warning = 0;
    main_sys_status.valve_state = 0;
    Valve_Drive(0);   // 泄压阀默认关闭（引脚 DO_OUTPUT_0/PE3 输出低电平）

    main_sys_status.adc_pressure = 0.0f;
    main_sys_status.adc_oil_temp = 0.0f;
    main_sys_status.liquid_level_pct = 0.0f;

    main_sys_status.sensor_status = SENSOR_STATUS_FAULT;
    main_sys_status.sys_fault_bit = 0;

    main_sys_status.motor_status.comm_lost = 0U;
    main_sys_status.motor_status.state = MOTOR_STATE_STOP;
    main_sys_status.motor_status.err_code = 0;
    main_sys_status.motor_status.status_word = 0;
    main_sys_status.motor_status.real_ctrl_mode = 0xFF;
    main_sys_status.motor_status.set_ctrl_mode = MOTOR_CTRL_MODE_VEL;
    main_sys_status.motor_status.target_speed = 0;
    main_sys_status.motor_status.target_acc = 100;
    main_sys_status.motor_status.target_dec = 100;
    main_sys_status.motor_status.actual_speed = 0;

    main_sys_status.fault_reset_req = 0U;
    main_sys_status.manual_set_speed = 0.0f;
}

/* =========泄压阀 GPIO 驱动=========
 * 阀门接 DO_OUTPUT_0 (PE3)，gpio.c 已配置为 GPIO_MODE_OUTPUT_PP。
 * valve_state: 0=关闭(低电平) / 1=打开(高电平)，开阀=高电平(用户确认)。
 * PE3 原被 step_motor 用作电机 RDY 输入读取，已在 step_motor.c 屏蔽。
 */
void Valve_Drive(uint8_t open)
{
    HAL_GPIO_WritePin(DO_OUTPUT_0_GPIO_Port, DO_OUTPUT_0_Pin,
                      (open != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

// =========配置恢复出厂函数【解决你Cfg_SetDefault缺失报错】=========
int Cfg_SetDefault(void)
{
    sys_cfg.ref_pressure = 5.0f;
    sys_cfg.max_pressure = 20.0f;
    sys_cfg.press_hysteresis = 0.3f;
    sys_cfg.overpress_margin = 10.0f;

    sys_cfg.max_motor_speed = 2000.0f;
    sys_cfg.min_motor_speed = 100.0f;

    sys_cfg.max_oil_temp = 85.0f;
    sys_cfg.min_liquid_level = 20.0f;

    sys_cfg.pid_kp = 2.5f;
    sys_cfg.pid_ki = 0.08f;
    sys_cfg.pid_kd = 0.12f;

    sys_cfg.modbus_addr = 1;
    sys_cfg.modbus_baud_sel = 6; //115200

    sys_cfg.press_cal_4ma_raw  = 12710.0f;
    sys_cfg.press_cal_20ma_raw = 63549.0f;

    sys_cfg.temp_cal_4ma_raw   = 12710.0f;
    sys_cfg.temp_cal_20ma_raw  = 63549.0f;

    return 0;
}

uint8_t SysStatus_ReadSnapshot(_SYS_STATUS *dst)
{
    if(dst == NULL)
        return 0;

    // 互斥锁正常存在
    if(sys_status_mutex != NULL)
    {
        if(osMutexAcquire(sys_status_mutex, pdMS_TO_TICKS(20U)) == osOK)
        {
            *dst = main_sys_status;
            osMutexRelease(sys_status_mutex);
            return 1;
        }
        else
        {
            // 获取锁超时：降级裸拷贝，记录警告日志
            LOG_logic("sys mutex acquire timeout, fallback raw read");
        }
    }
    else
    {
        // 互斥锁未创建，降级裸拷贝
        LOG_logic("sys mutex NULL, fallback raw read");
    }

    // 兜底降级：不加锁直接拷贝（存在撕裂风险，仅异常兜底）
    *dst = main_sys_status;
    return 0;
}

uint8_t SysStatus_WriteSnapshot(_SYS_STATUS *src)
{
    if(src == NULL)
        return 0;

    if(sys_status_mutex != NULL)
    {
        if(osMutexAcquire(sys_status_mutex, pdMS_TO_TICKS(20U)) == osOK)
        {
            main_sys_status = *src;
            osMutexRelease(sys_status_mutex);
            return 1;
        }
        else
        {
            LOG_logic("sys mutex acquire timeout, write abort");
            // 写入操作风险极高！锁拿不到，禁止裸写，直接放弃本次更新
            return 0;
        }
    }
    else
    {
        LOG_logic("sys mutex NULL, write abort");
        // 锁不存在，禁止裸写全局状态，防止多任务并发篡改
        return 0;
    }
}

uint32_t stop_enter_tick = 0U;       // 进入停机状态时刻tick
void MainLogicTask(void *argument)
{
    uint8_t last_sys_state = 0xFF; //非法初始状态，强制第一次进场条件成立

    // 模拟量保护防抖计数器
    static uint8_t overpress_cnt     = 0;
    static uint8_t oil_temp_over_cnt = 0;
    static uint8_t low_liquid_cnt    = 0;
    static uint8_t severe_overpress_flag = 0;
    static uint8_t last_fault_reset_req = 0U;
    static uint8_t last_global_fault_bit = 0U;// 全局故障位图历史缓存，用于检测故障变化

    // 先延时4秒，等待电机上电自检完成
    osDelay(pdMS_TO_TICKS(4000));

    for(;;)
    {
        // 一轮循环只读取一次全局快照，用于逻辑判断（禁止直接拿它拷贝写入）
        _SYS_STATUS sys_tmp;
        uint8_t read_ok = SysStatus_ReadSnapshot(&sys_tmp);
        if(read_ok == 0)
        {
            osDelay(pdMS_TO_TICKS(20));
            continue;
        }

        float curr_p                = sys_tmp.adc_pressure;
        float ref_p                 = sys_cfg.ref_pressure;
        float max_pressure          = sys_cfg.max_pressure;
        float overpress_margin      = sys_cfg.overpress_margin;
        float severe_overpress_thr  = max_pressure * (1.0f + overpress_margin / 100.0f);
        float max_oil_temp          = sys_cfg.max_oil_temp;
        float min_liquid_level      = sys_cfg.min_liquid_level;
        float PRESS_HYSTERESIS      = sys_cfg.press_hysteresis;

        uint8_t motor_fault           = 0;//电机故障标志
        uint8_t oil_temp_fault        = 0;//油温过高锁死故障标志
        uint8_t low_liquid_fault      = 0;//油液过低故障标志
        uint8_t motor_comm_lost_flag  = sys_tmp.motor_status.comm_lost;//电机通信故障标志
        uint8_t motor_mode_abnormal   = 0U;// 伺服模式校验标志

        // 标记：是否锁定设置模式
        uint8_t lock_setting_mode = (sys_tmp.sys_start == SYS_STATE_SETTING);

        //不在故障状态，如果fault_reset_req被置1，直接清零，不允许残留复位标记
        if(sys_tmp.sys_start != SYS_STATE_FAULT && sys_tmp.fault_reset_req == 1U)
        {
            _SYS_STATUS write_tmp;
            if(SysStatus_ReadSnapshot(&write_tmp))
            {
                write_tmp.fault_reset_req = 0U;
                SysStatus_WriteSnapshot(&write_tmp);
            }
        }

        //故障复位上升沿检测
        uint8_t fault_reset_trig = 0U;
        if((sys_tmp.fault_reset_req == 1U) && (last_fault_reset_req == 0U))
        {
            fault_reset_trig = 1U;
        }
        last_fault_reset_req = sys_tmp.fault_reset_req;

        //====================故障位图统一汇总【唯一生成sys_fault_bit】====================
        uint8_t local_fault_bit = 0U;

        //1.外部上报：传感器断线故障（ADC/Modbus任务上报到g_fault_report）
        if(g_fault_report.flg_press_sensor_err)//压力传感器故障
            local_fault_bit |= SENSOR_ERR_PRESSURE;
        if(g_fault_report.flg_temp_sensor_err)//油温传感器故障
            local_fault_bit |= SENSOR_ERR_OIL_TEMP;
        if(g_fault_report.flg_liquid_sensor_err)//液位传感器故障
            local_fault_bit |= SENSOR_ERR_LIQUID;

        //2.严重超压判定（二级保护：触发泄压阀，可自动恢复）
        if(curr_p >= severe_overpress_thr)
        {
            overpress_cnt++;
            if(overpress_cnt >= FAULT_DEBOUNCE_CNT)
            {
                severe_overpress_flag = 1;
                local_fault_bit |= SYS_ERR_SEVERE_OVERPRESS;
            }
        }
        else
        {
            overpress_cnt = 0;
            severe_overpress_flag = 0;
        }

        //3.油液过温保护（锁死故障，不可自动恢复）
        if(sys_tmp.adc_oil_temp >= max_oil_temp)
        {
            oil_temp_over_cnt++;
            if(oil_temp_over_cnt >= FAULT_DEBOUNCE_CNT)
            {
                oil_temp_fault = 1;
                local_fault_bit |= SYS_ERR_OIL_TEMP_OVER;
            }
        }
        else
        {
            oil_temp_over_cnt = 0;
        }

        //4.低液位保护（锁死故障）液位传感器异常时，不做液位判定，直接跳过
        if(g_fault_report.flg_liquid_sensor_err == 0U)
        {
            if(sys_tmp.liquid_level_pct < min_liquid_level)
            {
                low_liquid_cnt++;
                if(low_liquid_cnt >= FAULT_DEBOUNCE_CNT)
                {
                    low_liquid_fault = 1;
                    local_fault_bit |= SYS_ERR_LOW_LIQUID;
                }
            }
            else
            {
                low_liquid_cnt = 0;
            }
        }
        else
        {
            //传感器故障，防抖计数器清零，防止恢复后瞬间触发故障
            low_liquid_cnt = 0;
        }

        //6.电机通讯丢失故障
        if(motor_comm_lost_flag)
        {
            local_fault_bit |= SYS_ERR_MOTOR_COMM_LOST;
        }

        //电机通讯丢失：驱动器故障、伺服模式校验全部跳过
        if(motor_comm_lost_flag == 0U)
        {
            if(sys_tmp.motor_status.set_ctrl_mode != MOTOR_CTRL_MODE_VEL)
            {
                motor_mode_abnormal = 1U;
            }

            //5.电机驱动器本体故障判定
            if((sys_tmp.motor_status.err_code != 0U) ||
               ((sys_tmp.motor_status.status_word & STATUS_BIT_FAULT) != 0U))
            {
                motor_fault = 1;
                local_fault_bit |= SYS_ERR_MOTOR_DRIVER;
            }
        }

        //================传感器综合状态判定================
        SENSOR_STATUS_E calc_sensor_status;
        // 只判断3个传感器故障bit
        const uint8_t sensor_fault_mask = (SENSOR_ERR_PRESSURE | SENSOR_ERR_OIL_TEMP | SENSOR_ERR_LIQUID);
        if((local_fault_bit & sensor_fault_mask) != 0U)
        {
            calc_sensor_status = SENSOR_STATUS_FAULT;
        }
        else
        {
            calc_sensor_status = SENSOR_STATUS_READY;
        }

                // ===================== 故障变更记录报警逻辑 =====================
        // 触发条件：故障位图发生变化，且当前仍存在故障（故障消失不记录）
        uint8_t fault_change_trig = ((local_fault_bit != last_global_fault_bit) && (local_fault_bit != 0U));
        if(fault_change_trig)
        {
            // 故障发生/故障切换，写入一条报警记录
            uint16_t write_idx = g_alarm_record_cnt;
            if(write_idx >= ALARM_RECORD_MAX_CNT)
            {
                // 记录满了，丢弃最早一条，整体前移覆盖
                memmove(&g_alarm_records[0], &g_alarm_records[1], sizeof(AlarmRecord) * (ALARM_RECORD_MAX_CNT - 1));
                write_idx = ALARM_RECORD_MAX_CNT - 1;
                g_alarm_record_cnt = ALARM_RECORD_MAX_CNT;
            }
            else
            {
                g_alarm_record_cnt++;
            }

            // 填充当前故障信息
            AlarmRecord *new_rec = &g_alarm_records[write_idx];
            new_rec->fault_bit = local_fault_bit;
            new_rec->tick = xTaskGetTickCount();

            // 从系统tick换算时分秒
            uint32_t total_sec = new_rec->tick / configTICK_RATE_HZ;
            new_rec->hour = (total_sec / 3600) % 24;
            new_rec->min = (total_sec % 3600) / 60;
            new_rec->sec = total_sec % 60;
        }
        // 更新上一周期故障位图，用于下一周期边沿判断
        last_global_fault_bit = local_fault_bit;

        // =========故障位图 + sensor_status写入：读快照，有变化才写 =========
        _SYS_STATUS write_tmp;
        if(SysStatus_ReadSnapshot(&write_tmp))
        {
            uint8_t fault_bit_changed = (local_fault_bit != write_tmp.sys_fault_bit);
            uint8_t sensor_stat_changed = (calc_sensor_status != write_tmp.sensor_status);

            if(fault_bit_changed || sensor_stat_changed)
            {
                write_tmp.sys_fault_bit = local_fault_bit;
                write_tmp.sensor_status = calc_sensor_status;

                uint8_t write_ret = SysStatus_WriteSnapshot(&write_tmp);
                if(write_ret == 0)
                {
                    LOG_logic("Fault/sensor_status write mutex timeout");
                }
            }
        }

        //汇总全部故障
        uint8_t auto_fault_exist  = (motor_fault || motor_comm_lost_flag || severe_overpress_flag
                                    || motor_mode_abnormal
                                    || g_fault_report.flg_press_sensor_err
                                    || g_fault_report.flg_temp_sensor_err
                                    || g_fault_report.flg_liquid_sensor_err);

        uint8_t lock_fault_exist  = (oil_temp_fault || low_liquid_fault);

        
        // 自检完成后，运行阶段全局故障联锁
        // 修改：设置模式下故障不强制切FAULT，保持SETTING
        if(auto_fault_exist || lock_fault_exist)
        {
            // 当前是设置模式 / 当前已是故障态，跳过故障自动跳转
            if(sys_tmp.sys_start != SYS_STATE_SETTING && sys_tmp.sys_start != SYS_STATE_FAULT)
            {
                _SYS_STATUS write_tmp;
                if(SysStatus_ReadSnapshot(&write_tmp))
                {
                    if(write_tmp.sys_start != SYS_STATE_FAULT)
                    {
                        write_tmp.sys_start = SYS_STATE_FAULT;
                        if(SysStatus_WriteSnapshot(&write_tmp))
                        {
                            SysStatus_ReadSnapshot(&sys_tmp);
                        }
                    }
                }
            }
        }

        // ============ 正常运行状态机：分层拦截设置模式 ============
        if(lock_setting_mode)
        {
            // 锁定设置模式：仅执行SETTING分支，屏蔽所有压力自动切换逻辑
            switch(sys_tmp.sys_start)
            {
                case SYS_STATE_SETTING:
                {
                    if(last_sys_state != SYS_STATE_SETTING)
                    {
                        LOG_logic("Enter SETTING Mode (Locked, auto-switch disabled)");
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_update = 0;
                            if((write_tmp.motor_status.state != MOTOR_STATE_FAULT) && (write_tmp.motor_status.state != MOTOR_STATE_STOP))
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_STOP;
                                need_update = 1;
                            }
                            if(need_update)
                            {
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        PID_Reset(&g_pressure_pid);
                        last_sys_state = SYS_STATE_SETTING;
                    }

                    // 设置模式常驻逻辑：无任何自动状态跳转
                    // 可选增强：设置模式下存在故障时关停电机阀门，不修改系统状态
                    if(auto_fault_exist || lock_fault_exist)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_mod = 0;
                            if(write_tmp.motor_status.state != MOTOR_STATE_STOP)
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_STOP;
                                need_mod = 1;
                            }
                            if(need_mod)
                            {
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
        else
        {
            // 非设置模式：完整执行原有全部状态自动切换逻辑
            switch(sys_tmp.sys_start)
            {
                case SYS_STATE_STOP:
                {
                    if(last_sys_state != SYS_STATE_STOP)
                    {
                        LOG_logic("Enter STOP Mode");
                        stop_enter_tick = osKernelGetTickCount();
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_update = 0;
                            if(write_tmp.valve_state != 0)
                            {
                                write_tmp.valve_state = 0;
                                Valve_Drive(0);
                                need_update = 1;
                            }
                            if((write_tmp.motor_status.state != MOTOR_STATE_FAULT) && (write_tmp.motor_status.state != MOTOR_STATE_STOP))
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_STOP;
                                need_update = 1;
                            }
                            if(need_update)
                            {
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        PID_Reset(&g_pressure_pid);
                        show_oled_msg(OLED_MSG_UPDATE, "System stop, standby");

                        last_sys_state = SYS_STATE_STOP;
                    }

                    uint32_t now_tick = osKernelGetTickCount();
                    uint32_t elapsed_tick = now_tick - stop_enter_tick;
                    if(elapsed_tick < pdMS_TO_TICKS(STOP_STABLE_DELAY_MS))//等待100ms，确保电机停止
                    {
                        break;
                    }

                    // 压力低于目标启动加压
                    if(curr_p < ref_p - PRESS_HYSTERESIS && curr_p > 0.2f)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.sys_start != SYS_STATE_PRESS)
                            {
                                write_tmp.sys_start = SYS_STATE_PRESS;
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    // 压力过高进入泄压
                    if(curr_p >= max_pressure)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.sys_start != SYS_STATE_RELEASE)
                            {
                                write_tmp.sys_start = SYS_STATE_RELEASE;
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    break;
                }
                case SYS_STATE_PRESS:
                {
                    if(last_sys_state != SYS_STATE_PRESS)
                    {
                        LOG_logic("Enter PRESS Mode");
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.motor_status.state != MOTOR_STATE_RUN)
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_RUN;
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        show_oled_msg(OLED_MSG_UPDATE, "Pressure boosting");

                        last_sys_state = SYS_STATE_PRESS;
                    }

                    if(curr_p >= ref_p)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.sys_start != SYS_STATE_HOLD)
                            {
                                write_tmp.sys_start = SYS_STATE_HOLD;
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    if(curr_p >= max_pressure)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.sys_start != SYS_STATE_STOP)
                            {
                                write_tmp.sys_start = SYS_STATE_STOP;
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    break;
                }

                case SYS_STATE_HOLD:
                {
                    if(last_sys_state != SYS_STATE_HOLD)
                    {
                        LOG_logic("Enter HOLD Mode");
                        PID_Reset(&g_pressure_pid);
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.motor_status.state != MOTOR_STATE_RUN)
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_RUN;
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        show_oled_msg(OLED_MSG_UPDATE, "Pressure holding");
                        last_sys_state = SYS_STATE_HOLD;
                    }

                    if(curr_p < ref_p - PRESS_HYSTERESIS)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.sys_start != SYS_STATE_PRESS)
                            {
                                write_tmp.sys_start = SYS_STATE_PRESS;
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    if(curr_p >= max_pressure)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.sys_start != SYS_STATE_STOP)
                            {
                                write_tmp.sys_start = SYS_STATE_STOP;
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    break;
                }

                case SYS_STATE_RELEASE:
                {
                    if(last_sys_state != SYS_STATE_RELEASE)
                    {
                        LOG_logic("Enter RELEASE Mode, Open Valve");
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_update = 0;
                            if(write_tmp.valve_state != 1)
                            {
                                write_tmp.valve_state = 1;
                                Valve_Drive(1);
                                need_update = 1;
                            }
                            if((write_tmp.motor_status.state != MOTOR_STATE_FAULT) && (write_tmp.motor_status.state != MOTOR_STATE_STOP))
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_STOP;
                                need_update = 1;
                            }
                            if(need_update)
                            {
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        PID_Reset(&g_pressure_pid);
                        show_oled_msg(OLED_MSG_UPDATE, "Pressure releasing");
                        last_sys_state = SYS_STATE_RELEASE;
                    }

                    if(curr_p <= 0.2f)
                    {
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_update = 0;
                            if(write_tmp.valve_state != 0)
                            {
                                write_tmp.valve_state = 0;
                                Valve_Drive(0);
                                need_update = 1;
                            }
                            if(write_tmp.sys_start != SYS_STATE_STOP)
                            {
                                write_tmp.sys_start = SYS_STATE_STOP;
                                need_update = 1;
                            }
                            if(need_update)
                            {
                                if(SysStatus_WriteSnapshot(&write_tmp))
                                {
                                    SysStatus_ReadSnapshot(&sys_tmp);
                                }
                            }
                        }
                    }
                    break;
                }

                case SYS_STATE_SETTING:
                {
                    if(last_sys_state != SYS_STATE_SETTING)
                    {
                        LOG_logic("Enter SETTING Mode");
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_update = 0;
                            if((write_tmp.motor_status.state != MOTOR_STATE_FAULT) && (write_tmp.motor_status.state != MOTOR_STATE_STOP))
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_STOP;
                                need_update = 1;
                            }
                            if(need_update)
                            {
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        PID_Reset(&g_pressure_pid);
                        last_sys_state = SYS_STATE_SETTING;
                    }
                    break;
                }
                case SYS_STATE_FAULT:
                {
                    static uint32_t fault_switch_tick = 0;
                    static uint8_t fault_display_idx = 0;
                    const uint32_t FAULT_SWITCH_PERIOD = pdMS_TO_TICKS(1000);
                    uint32_t now_tick = osKernelGetTickCount();

                    if(last_sys_state != SYS_STATE_FAULT)
                    {
                        LOG_logic("System Enter FAULT State");
                        fault_switch_tick = now_tick;
                        fault_display_idx = 0;
                        _SYS_STATUS write_tmp;
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            uint8_t need_update = 0;
                            if(write_tmp.motor_status.state != MOTOR_STATE_FAULT)
                            {
                                write_tmp.motor_status.state = MOTOR_STATE_FAULT;
                                need_update = 1;
                            }
                            if(need_update)
                            {
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }
                        PID_Reset(&g_pressure_pid);

                        last_sys_state = SYS_STATE_FAULT;
                    }

                    // 构造故障文本列表
                    #define MAX_ACTIVE_FAULT 9
                    const char *fault_list[MAX_ACTIVE_FAULT] = {0};
                    uint8_t fault_cnt = 0;

                    // 电机通讯丢失：只屏蔽驱动故障、模式异常，其余故障照常显示
                    if(motor_comm_lost_flag)
                    {
                        fault_list[fault_cnt++] = "Motor Comm Lost";
                        // ========== 跳过：Motor Driver Fault、Motor Mode Abnormal ==========
                    }
                    else
                    {
                        if((local_fault_bit & SYS_ERR_MOTOR_DRIVER) && fault_cnt < MAX_ACTIVE_FAULT)
                            fault_list[fault_cnt++] = "Motor Driver Fault";
                        if(motor_mode_abnormal && fault_cnt < MAX_ACTIVE_FAULT)
                            fault_list[fault_cnt++] = "Motor Mode Abnormal";
                    }

                    // 下面这些【无论通讯是否丢失，全部照常加入故障列表】
                    if((local_fault_bit & SENSOR_ERR_LIQUID) && fault_cnt < MAX_ACTIVE_FAULT)
                        fault_list[fault_cnt++] = "Liquid Sensor Fault";
                    if((local_fault_bit & SENSOR_ERR_PRESSURE) && fault_cnt < MAX_ACTIVE_FAULT)
                        fault_list[fault_cnt++] = "Pressure Sensor Fault";
                    if((local_fault_bit & SENSOR_ERR_OIL_TEMP) && fault_cnt < MAX_ACTIVE_FAULT)
                        fault_list[fault_cnt++] = "OilTemp Sensor Fault";
                    if((local_fault_bit & SYS_ERR_SEVERE_OVERPRESS) && fault_cnt < MAX_ACTIVE_FAULT)
                        fault_list[fault_cnt++] = "Overpressure Fault";
                    if((local_fault_bit & SYS_ERR_OIL_TEMP_OVER) && fault_cnt < MAX_ACTIVE_FAULT)
                        fault_list[fault_cnt++] = "Oil Temp Overheat";
                    if((local_fault_bit & SYS_ERR_LOW_LIQUID) && fault_cnt < MAX_ACTIVE_FAULT)
                        fault_list[fault_cnt++] = "Low Liquid Level";

                    if(fault_cnt == 0)
                        fault_list[fault_cnt++] = "Unknown Fault";

                    // 定时轮换故障显示
                    if((now_tick - fault_switch_tick) >= FAULT_SWITCH_PERIOD)
                    {
                        fault_switch_tick = now_tick;
                        fault_display_idx++;
                        if(fault_display_idx >= fault_cnt)
                            fault_display_idx = 0;
                        show_oled_msg(OLED_MSG_ERROR, fault_list[fault_display_idx]);
                    }
                    _SYS_STATUS write_tmp;
                    if(SysStatus_ReadSnapshot(&write_tmp))
                    {
                        uint8_t target_valve = severe_overpress_flag ? 1 : 0;
                        if(write_tmp.valve_state != target_valve)
                        {
                            write_tmp.valve_state = target_valve;
                            Valve_Drive(target_valve);
                            SysStatus_WriteSnapshot(&write_tmp);
                        }
                    }
                    // 故障退出逻辑
                    if(lock_fault_exist == 0)// 无锁故障
                    {
                        if(auto_fault_exist == 0)// 无自动故障
                        {
                            LOG_logic("All auto-clear faults recovered, exit FAULT");
                            {
                                _SYS_STATUS write_tmp;
                                if(SysStatus_ReadSnapshot(&write_tmp))
                                {
                                    uint8_t need_update = 0;
                                    if(write_tmp.sys_start != SYS_STATE_STOP)
                                    {
                                        write_tmp.sys_start = SYS_STATE_STOP;
                                        need_update = 1;
                                    }
                                    if(write_tmp.motor_status.state != MOTOR_STATE_WAIT)
                                    {
                                        write_tmp.motor_status.state = MOTOR_STATE_WAIT;
                                        need_update = 1;
                                    }
                                    if(write_tmp.fault_reset_req != 0)
                                    {
                                        write_tmp.fault_reset_req = 0;
                                        need_update = 1;
                                    }
                                    if(write_tmp.sys_fault_bit != 0U)
                                    {
                                        write_tmp.sys_fault_bit = 0U;
                                        need_update = 1;
                                    }
                                    if(need_update)
                                    {
                                        if(SysStatus_WriteSnapshot(&write_tmp))
                                        {
                                            memset(&g_fault_report,0,sizeof(FAULT_REPORT_T));
                                            SysStatus_ReadSnapshot(&sys_tmp);
                                        }
                                    }
                                }
                            }
                            show_oled_msg(OLED_MSG_UPDATE, "Fault cleared, standby");
                        }
                    }
                    //2.存在锁死故障（油温/低液位），必须等待手动复位触发
                    if(fault_reset_trig == 1U)// 故障复位上升沿触发
                    {
                        _SYS_STATUS write_tmp;
                        //先清除复位请求标志
                        if(SysStatus_ReadSnapshot(&write_tmp))
                        {
                            if(write_tmp.fault_reset_req != 0U)
                            {
                                write_tmp.fault_reset_req = 0U;
                                SysStatus_WriteSnapshot(&write_tmp);
                            }
                        }

                        //复位触发之后，再次读取最新快照，重新评估故障
                        SysStatus_ReadSnapshot(&sys_tmp);

                        //再次计算锁死故障，判断是否还存在硬件故障条件
                        uint8_t curr_lock_fault = 0;
                        if(sys_tmp.adc_oil_temp >= sys_cfg.max_oil_temp)
                            curr_lock_fault = 1;
                        if(sys_tmp.liquid_level_pct <= sys_cfg.min_liquid_level)
                            curr_lock_fault = 1;

                        if(curr_lock_fault == 0U)
                        {
                            LOG_logic("Manual reset success, exit FAULT");
                            if(SysStatus_ReadSnapshot(&write_tmp))
                            {
                                uint8_t need_update = 0;
                                if(write_tmp.sys_start != SYS_STATE_STOP)
                                {
                                    write_tmp.sys_start = SYS_STATE_STOP;
                                    need_update = 1;
                                }
                                if(write_tmp.motor_status.state != MOTOR_STATE_WAIT)
                                {
                                    write_tmp.motor_status.state = MOTOR_STATE_WAIT;
                                    need_update = 1;
                                }
                                if(write_tmp.sys_fault_bit != 0U)
                                {
                                    write_tmp.sys_fault_bit = 0U;
                                    need_update = 1;
                                }
                                if(need_update)
                                {
                                    if(SysStatus_WriteSnapshot(&write_tmp))
                                    {
                                        memset(&g_fault_report,0,sizeof(FAULT_REPORT_T));
                                        SysStatus_ReadSnapshot(&sys_tmp);
                                    }
                                }
                            }
                            show_oled_msg(OLED_MSG_UPDATE, "Fault cleared, standby");
                        }
                        else
                        {
                            LOG_logic("Reset reject: Lock fault still exists");
                            show_oled_msg(OLED_MSG_ERROR, "Fault still exists");
                        }
                    }
                    break;
                }
                default:
                {
                    LOG_logic("Unknown system state, force STOP");
                    _SYS_STATUS write_tmp;
                    if(SysStatus_ReadSnapshot(&write_tmp))
                    {
                        uint8_t need_update = 0;
                        if(write_tmp.sys_start != SYS_STATE_STOP)
                        {
                            write_tmp.sys_start = SYS_STATE_STOP;
                            need_update = 1;
                        }
                        if((write_tmp.motor_status.state != MOTOR_STATE_FAULT) && (write_tmp.motor_status.state != MOTOR_STATE_STOP))
                        {
                            write_tmp.motor_status.state = MOTOR_STATE_STOP;
                            need_update = 1;
                        }
                        if(need_update)
                        {
                            if(SysStatus_WriteSnapshot(&write_tmp))
                            {
                                SysStatus_ReadSnapshot(&sys_tmp);
                            }
                        }
                    }
                    PID_Reset(&g_pressure_pid);
                    break;
                }
            }
        }

        osDelay(pdMS_TO_TICKS(200));
    }
}



