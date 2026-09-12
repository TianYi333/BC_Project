#ifndef ADC_COLLECT_H
#define ADC_COLLECT_H

#include "project_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include <string.h>
#include "main_logic.h"

// ADC滑动平均滤波深度
#define ADC_FILTER_DEPTH    8U

// ADC传感器限值
#define PRESS_MIN_LIMIT     0.0f
#define PRESS_MAX_LIMIT     25.0f

#define OIL_TEMP_MIN        -20.0f
#define OIL_TEMP_MAX        120.0f

// 故障防抖连续次数：连续3次异常才判定故障，避免ADC毛刺误触发
#define SENSOR_ERR_FILTER_CNT    3U

extern volatile __attribute__((section(".ram_d3"))) uint16_t adc_raw_buf[4];
extern uint16_t adc_raw_backup[4];
// 数据就绪标记
extern volatile uint8_t adc_data_ready;
// 物理量结果
extern float adc_current[2];  // ch2 ch3 两路电流

// 滑动平均滤波缓存
extern float press_filter_buf[ADC_FILTER_DEPTH];
extern float temp_filter_buf[ADC_FILTER_DEPTH];
extern uint8_t press_filter_idx;
extern uint8_t temp_filter_idx;

void ADCTask(void *argument);
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc);









#endif
