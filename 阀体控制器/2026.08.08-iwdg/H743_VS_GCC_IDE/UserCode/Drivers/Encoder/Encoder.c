/**
 * @file Encoder.c
 * @brief 编码器驱动实现文件
 * @author 梁伟
 * @date 2024-06-15
 */

#include "Encoder.h"
#include "msg_queue.h"
#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "buzzer.h"

/**
 * @brief 全局编码器状态变量
 */
EncoderState g_encoderState = ENCODER_NONE;

uint8_t buzz_status = 0;

BaseType_t xHigherPriorityTaskWoken = pdFALSE;

// 长按检测相关变量
static uint32_t button_press_time = 0;
static uint8_t button_pressed = 0;
static const uint32_t LONG_PRESS_THRESHOLD = 1000; // 长按阈值（毫秒）

/**
 * @brief 编码器中断回调函数
 * @param GPIO_Pin 触发中断的GPIO引脚
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  static uint8_t lastA = 0;
  static uint8_t lastB = 0;
  static uint8_t lastP = 0;
  uint8_t currentA = HAL_GPIO_ReadPin(ENCODER_A_PORT, ENCODER_A_PIN);
  uint8_t currentB = HAL_GPIO_ReadPin(ENCODER_B_PORT, ENCODER_B_PIN);
  uint8_t currentP = HAL_GPIO_ReadPin(ENCODER_P_PORT, ENCODER_P_PIN);

  // 旋转处理
  if (GPIO_Pin == ENCODER_A_PIN || GPIO_Pin == ENCODER_B_PIN)
  {
    if (lastA == currentA && lastB == currentB)
    {
      return; // 防抖处理
    }

    if (lastA == 0 && currentA == 1)
    { // A上升沿
      if (currentB == 0)
      {
        // 顺时针旋转
        g_encoderState = ENCODER_CW;
        xQueueSendFromISR(QUEUE_KEY, &g_encoderState, &xHigherPriorityTaskWoken);
        buzz_status = BUZZ_SHORT;
        xQueueSendFromISR(QUEUE_BUZZ, &buzz_status, &xHigherPriorityTaskWoken);
      }
      else
      {
        // 逆时针旋转
        g_encoderState = ENCODER_CCW;
        xQueueSendFromISR(QUEUE_KEY, &g_encoderState, &xHigherPriorityTaskWoken);
        buzz_status = BUZZ_SHORT;
        xQueueSendFromISR(QUEUE_BUZZ, &buzz_status, &xHigherPriorityTaskWoken);
      }
    }
    else if (lastB == 0 && currentB == 1)
    { // B上升沿
      if (currentA == 1)
      {
        // 顺时针旋转
        g_encoderState = ENCODER_CW;
        xQueueSendFromISR(QUEUE_KEY, &g_encoderState, &xHigherPriorityTaskWoken);
        buzz_status = BUZZ_SHORT;
        xQueueSendFromISR(QUEUE_BUZZ, &buzz_status, &xHigherPriorityTaskWoken);
      }
      else
      {
        // 逆时针旋转
        g_encoderState = ENCODER_CCW;
        xQueueSendFromISR(QUEUE_KEY, &g_encoderState, &xHigherPriorityTaskWoken);
        buzz_status = BUZZ_SHORT;
        xQueueSendFromISR(QUEUE_BUZZ, &buzz_status, &xHigherPriorityTaskWoken);
      }
    }
    lastA = currentA;
    lastB = currentB;
  }

  // 按钮处理
  if (GPIO_Pin == ENCODER_P_PIN)
  {
    // 按钮按下（下降沿）
    if (lastP == 1 && currentP == 0)
    {
      button_pressed = 1;
      button_press_time = xTaskGetTickCountFromISR(); // 使用ISR安全版本
    }
    // 按钮释放（上升沿）
    else if (lastP == 0 && currentP == 1)
    {
      if (button_pressed)
      {
        uint32_t current_time = xTaskGetTickCountFromISR();
        uint32_t press_duration = current_time - button_press_time;

        if (press_duration >= LONG_PRESS_THRESHOLD)
        {
          // 长按 - 返回操作
          g_encoderState = ENCODER_PUTH_LONG;
          buzz_status = BUZZ_LONG;
          xQueueSendFromISR(QUEUE_BUZZ, &buzz_status, &xHigherPriorityTaskWoken);
        }
        else
        {
          // 短按 - 确认选择
          g_encoderState = ENCODER_PUTH;
          buzz_status = BUZZ_SHORT;
          xQueueSendFromISR(QUEUE_BUZZ, &buzz_status, &xHigherPriorityTaskWoken);
        }
        xQueueSendFromISR(QUEUE_KEY, &g_encoderState, &xHigherPriorityTaskWoken);
        button_pressed = 0;
      }
    }
    lastP = currentP;
  }

  //处理传感器信号
  if(GPIO_Pin == DO_INPUT_0_Pin)
  {
      if (HAL_GPIO_ReadPin(DO_INPUT_0_GPIO_Port, DO_INPUT_0_Pin) == GPIO_PIN_SET)
      {
        uint8_t sensor_state = 1; // 传感器触发
        xQueueOverwriteFromISR(QUEUE_MET_SEN, &sensor_state, &xHigherPriorityTaskWoken);
      }
  }
  if(GPIO_Pin == DO_INPUT_1_Pin)
  {
      if (HAL_GPIO_ReadPin(DO_INPUT_1_GPIO_Port, DO_INPUT_1_Pin) == GPIO_PIN_SET)
      {
        uint8_t sensor_state = 2; // 传感器触发
        xQueueOverwriteFromISR(QUEUE_MET_SEN, &sensor_state, &xHigherPriorityTaskWoken);
      }
  }
}