/**
 * @file Encoder.h
 * @brief 编码器驱动头文件
 * @author 梁伟
 * @date 2024-06-15
 */

#ifndef ENCODER_H
#define ENCODER_H

#include "main.h"

/**
 * @brief 编码器状态枚举
 */
typedef enum {
  ENCODER_CW = 1,      /* 顺时针旋转 */
  ENCODER_CCW = -1,    /* 逆时针旋转 */
  ENCODER_NONE = 0,    /* 无旋转 */
  ENCODER_PUTH = 2,    /* 短按按钮 */
  ENCODER_PUTH_LONG = 3 /* 长按按钮 */
} EncoderState;

/**
 * @brief 旋转编码器GPIO配置（根据实际硬件连接修改）
 */
#define ENCODER_A_PIN EC11_A_Pin
#define ENCODER_A_PORT EC11_A_GPIO_Port
#define ENCODER_B_PIN EC11_B_Pin
#define ENCODER_B_PORT EC11_B_GPIO_Port
#define ENCODER_P_PIN EC11_P_Pin
#define ENCODER_P_PORT EC11_P_GPIO_Port

#endif /* ENCODER_H */