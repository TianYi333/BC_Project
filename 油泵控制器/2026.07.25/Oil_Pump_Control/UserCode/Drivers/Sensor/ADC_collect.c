#include "ADC_collect.h"

volatile __attribute__((section(".ram_d3"))) uint16_t adc_raw_buf[4];

uint16_t adc_raw_backup[4];
volatile uint8_t adc_data_ready = 0;
float adc_current[2] = {0};

void ADCTask(void *argument)
{
    LOG_Debug("buf_addr  = 0x%p", adc_raw_buf);
    LOG_Debug("ADC3_CR   = 0x%08x", ADC3->CR);
    LOG_Debug("ADC3_CFGR = 0x%08x", ADC3->CFGR);

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
            float new_press, new_temp;
            float press_current_mA;
            float temp_current_mA;

            // =========ch0 压力传感器【4~20mA / 0~40MPa / R=160Ω 换算】=========
            voltage = tmp_buf[0] * 3.3f / 65535.0f;
            press_current_mA = voltage / 160.0f * 1000.0f;
            // 4mA → 0MPa，20mA → 40MPa
            new_press = (press_current_mA - 4.0f) / 16.0f * 40.0f;
            // 最低限制0MPa，不会出现负数
            if(new_press < 0.0f)
            {
                new_press = 0.0f;
            }

            // =========ch1 油液温度NTC换算=========
            voltage = tmp_buf[1] * 3.3f / 65535.0f;
            temp_current_mA = voltage / 160.0f * 1000.0f;
            // 4mA → -50℃，20mA → 120℃，跨度170℃
            new_temp = (temp_current_mA - 4.0f) / 16.0f * 170.0f - 50.0f;

            // 界面限幅，抑制异常极端值
            if(new_temp < -60.0f)
                new_temp = -60.0f;
            if(new_temp > 130.0f)
                new_temp = 130.0f;

            // =========ch2 ch3 两路电流采样=========
            voltage = tmp_buf[2] * 3.3f / 65535.0f;
            adc_current[0] = voltage / 160.0f * 1000.0f;

            voltage = tmp_buf[3] * 3.3f / 65535.0f;
            adc_current[1] = voltage / 160.0f * 1000.0f;

            // =====================传感器故障防抖判断=====================
            // 压力变送器故障判定
            if(press_current_mA < 3.6f || press_current_mA > 21.0f)
            {
                if(press_err_cnt < SENSOR_ERR_FILTER_CNT)
                    press_err_cnt++;
            }
            else
            {
                press_err_cnt = 0; // 恢复正常，计数器清零
            }

            // 温度变送器故障判定
            if(temp_current_mA < 3.6f || temp_current_mA > 21.0f)
            {
                if(temp_err_cnt < SENSOR_ERR_FILTER_CNT)
                    temp_err_cnt++;
            }
            else
            {
                temp_err_cnt = 0;
            }

            // =====================批量更新全局系统状态（临界区包裹）=====================
            taskENTER_CRITICAL();
            // 先清除压力、温度故障位
            main_sys_status_1.sensor_err_bit &= ~(SENSOR_ERR_PRESSURE | SENSOR_ERR_OIL_TEMP);
            
            // 达到连续异常次数，置故障位
            if(press_err_cnt >= SENSOR_ERR_FILTER_CNT)
            {
                main_sys_status_1.sensor_err_bit |= SENSOR_ERR_PRESSURE;
            }
            if(temp_err_cnt >= SENSOR_ERR_FILTER_CNT)
            {
                main_sys_status_1.sensor_err_bit |= SENSOR_ERR_OIL_TEMP;
            }

            // 写入系统状态
            main_sys_status_1.adc_pressure = new_press;
            main_sys_status_1.adc_oil_temp = new_temp;

            // 更新综合传感器状态
            if(main_sys_status_1.sensor_err_bit != 0)
            {
                main_sys_status_1.sensor_status = SENSOR_STATUS_FAULT;
            }
            else
            {
                main_sys_status_1.sensor_status = SENSOR_STATUS_READY;
            }
            taskEXIT_CRITICAL();
            // ==============================================
        }

        // 调试打印，正式版本可按需注释
        LOG_Debug("P=%.2f OT=%.2f I0=%.2f I1=%.2f",
                main_sys_status_1.adc_pressure, main_sys_status_1.adc_oil_temp, adc_current[0], adc_current[1]);

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