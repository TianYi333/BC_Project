#include "ADC_collect.h"
#include "main_logic.h"
#include "log.h"
#include "cmsis_os2.h"
#include "string.h"

volatile __attribute__((section(".ram_d3"))) uint16_t adc_raw_buf[4];

uint16_t adc_raw_backup[4];
volatile uint8_t adc_data_ready = 0;
float adc_current[2] = {0};

// 滑动平均滤波缓冲区
float press_filter_buf[ADC_FILTER_DEPTH] = {0.0f};
float temp_filter_buf[ADC_FILTER_DEPTH]  = {0.0f};
uint8_t press_filter_idx = 0;
uint8_t temp_filter_idx  = 0;

void ADCTask(void *argument)
{
    // LOG_Debug("buf_addr  = 0x%p", adc_raw_buf);
    // LOG_Debug("ADC3_CR   = 0x%08lx", ADC3->CR);
    // LOG_Debug("ADC3_CFGR = 0x%08lx", ADC3->CFGR);

    // 故障防抖计数器（静态变量，任务内持久保存）
    static uint8_t press_err_cnt = 0;
    static uint8_t temp_err_cnt  = 0;
    for (;;)
    {
        if(adc_data_ready == 1)
        {
            uint16_t tmp_buf[4];

            // 临界区快速拷贝，防止中途标志被改写
            taskENTER_CRITICAL();
            memcpy(tmp_buf, adc_raw_backup, sizeof(tmp_buf));
            adc_data_ready = 0;
            taskEXIT_CRITICAL();

            float voltage;
            float raw_press_ma;
            float raw_temp_ma;
            float avg_press_ma;
            float avg_temp_ma;
            float cal_press_ma;
            float cal_temp_ma;
            float new_press, new_temp;
            SYS_CONFIG_T cfg_tmp;

            // =========ch0 压力传感器【4~20mA】原始电流计算=========
            voltage = tmp_buf[0] * 3.3f / 65535.0f;
            raw_press_ma = voltage / 160.0f * 1000.0f;

            // 压力滑动平均滤波
            press_filter_buf[press_filter_idx] = raw_press_ma;
            press_filter_idx = (press_filter_idx + 1) % ADC_FILTER_DEPTH;
            avg_press_ma = 0.0f;
            for(uint8_t i = 0; i < ADC_FILTER_DEPTH; i++)
            {
                avg_press_ma += press_filter_buf[i];
            }
            avg_press_ma /= ADC_FILTER_DEPTH;

            // 两点校准修正压力电流
            SysConfig_ReadSnapshot(&cfg_tmp);
            float press_k = (20.0f - 4.0f) / (cfg_tmp.press_cal_20ma_raw - cfg_tmp.press_cal_4ma_raw);
            float press_off = 4.0f - press_k * cfg_tmp.press_cal_4ma_raw;
            cal_press_ma = avg_press_ma * press_k + press_off;

            // 4mA → 0MPa，20mA → 40MPa
            new_press = (cal_press_ma - 4.0f) / 16.0f * 40.0f;
            // 最低限制0MPa，不会出现负数
            if(new_press < 0.0f)
            {
                new_press = 0.0f;
            }

            // =========ch1 油液温度变送器【4~20mA】原始电流=========
            voltage = tmp_buf[1] * 3.3f / 65535.0f;
            raw_temp_ma = voltage / 160.0f * 1000.0f;

            // 温度滑动平均滤波
            temp_filter_buf[temp_filter_idx] = raw_temp_ma;
            temp_filter_idx = (temp_filter_idx + 1) % ADC_FILTER_DEPTH;
            avg_temp_ma = 0.0f;
            for(uint8_t i = 0; i < ADC_FILTER_DEPTH; i++)
            {
                avg_temp_ma += temp_filter_buf[i];
            }
            avg_temp_ma /= ADC_FILTER_DEPTH;

            // 两点校准修正温度电流
            SysConfig_ReadSnapshot(&cfg_tmp);
            float temp_k = (20.0f - 4.0f) / (cfg_tmp.temp_cal_20ma_raw - cfg_tmp.temp_cal_4ma_raw);
            float temp_off = 4.0f - temp_k * cfg_tmp.temp_cal_4ma_raw;
            cal_temp_ma = avg_temp_ma * temp_k + temp_off;

            // 4mA → -50℃，20mA → 120℃，跨度170℃
            new_temp = (cal_temp_ma - 4.0f) / 16.0f * 170.0f - 50.0f;

            // 界面限幅，抑制异常极端值
            if(new_temp < -60.0f)
                new_temp = -60.0f;
            if(new_temp > 130.0f)
                new_temp = 130.0f;

            // =========ch2 ch3 两路电流采样（无校准滤波需求，保留原逻辑）=========
            voltage = tmp_buf[2] * 3.3f / 65535.0f;
            adc_current[0] = voltage / 160.0f * 1000.0f;

            voltage = tmp_buf[3] * 3.3f / 65535.0f;
            adc_current[1] = voltage / 160.0f * 1000.0f;

            // =====================传感器故障防抖判断（使用滤波后校准电流）=====================
            // 压力变送器故障判定
            if(cal_press_ma < 3.6f || cal_press_ma > 21.0f)
            {
                if(press_err_cnt < SENSOR_ERR_FILTER_CNT)
                    press_err_cnt++;
            }
            else
            {
                press_err_cnt = 0; // 恢复正常，计数器清零
            }

            // 温度变送器故障判定
            if(cal_temp_ma < 3.6f || cal_temp_ma > 21.0f)
            {
                if(temp_err_cnt < SENSOR_ERR_FILTER_CNT)
                    temp_err_cnt++;
            }
            else
            {
                temp_err_cnt = 0;
            }

            // =========【重点修改】只上报故障标记到全局结构体，不再修改sys_fault_bit=========
            if(press_err_cnt >= SENSOR_ERR_FILTER_CNT)
            {
                g_fault_report.flg_press_sensor_err = 1U;
            }
            else
            {
                g_fault_report.flg_press_sensor_err = 0U;
            }

            if(temp_err_cnt >= SENSOR_ERR_FILTER_CNT)
            {
                g_fault_report.flg_temp_sensor_err = 1U;
            }
            else
            {
                g_fault_report.flg_temp_sensor_err = 0U;
            }

            // =====================批量更新物理量（仅压力、温度数值，移除故障位操作）=====================
            _SYS_STATUS write_tmp;
            if(SysStatus_ReadSnapshot(&write_tmp))
            {
                // 默认先清空采集值
                write_tmp.adc_pressure = 0.0f;
                write_tmp.adc_oil_temp = 0.0f;

                // 根据故障标记填充数值
                if(g_fault_report.flg_press_sensor_err)
                {
                    write_tmp.adc_pressure = 0.0f;
                }
                else
                {
                    write_tmp.adc_pressure = new_press;
                }

                if(g_fault_report.flg_temp_sensor_err)
                {
                    write_tmp.adc_oil_temp = -60.0f;
                }
                else
                {
                    write_tmp.adc_oil_temp = new_temp;
                }
                // !!! 删除sensor_status更新，统一放到MainLogic
                SysStatus_WriteSnapshot(&write_tmp);
            }
            // ==============================================
        }

        // 调试打印，正式版本可按需注释
        _SYS_STATUS print_tmp;
        if(SysStatus_ReadSnapshot(&print_tmp))
        {
            //LOG_ADC("P=%.2f OT=%.2f I0=%.2f I1=%.2f",
            //        print_tmp.adc_pressure, print_tmp.adc_oil_temp, adc_current[0], adc_current[1]);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
    if(hadc->Instance == ADC3)
    {
        SCB_CleanInvalidateDCache_by_Addr((uint32_t *)adc_raw_buf, sizeof(adc_raw_buf));
        UBaseType_t uxSavedInterruptStatus;
        uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
        for(uint8_t i = 0; i < 4; i++)
        {
            adc_raw_backup[i] = adc_raw_buf[i];
        }
        adc_data_ready = 1;
        taskEXIT_CRITICAL_FROM_ISR(uxSavedInterruptStatus);
    }
}

