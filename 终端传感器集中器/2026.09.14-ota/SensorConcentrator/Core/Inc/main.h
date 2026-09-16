/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define UART8_RTS_Pin GPIO_PIN_2
#define UART8_RTS_GPIO_Port GPIOE
#define LAN_NRST_Pin GPIO_PIN_0
#define LAN_NRST_GPIO_Port GPIOA
#define UART7_RTS_Pin GPIO_PIN_5
#define UART7_RTS_GPIO_Port GPIOA
#define LED2_Pin GPIO_PIN_8
#define LED2_GPIO_Port GPIOE
#define USART3_RTS_Pin GPIO_PIN_13
#define USART3_RTS_GPIO_Port GPIOE
#define UART5_RTS_Pin GPIO_PIN_15
#define UART5_RTS_GPIO_Port GPIOE
#define USART1_RTS_Pin GPIO_PIN_9
#define USART1_RTS_GPIO_Port GPIOD
#define USART6_RTS_Pin GPIO_PIN_5
#define USART6_RTS_GPIO_Port GPIOG
#define SWDIO_Pin GPIO_PIN_13
#define SWDIO_GPIO_Port GPIOA
#define SWCLK_Pin GPIO_PIN_14
#define SWCLK_GPIO_Port GPIOA
#define UART4_RTS_Pin GPIO_PIN_15
#define UART4_RTS_GPIO_Port GPIOA
#define USART2_RTS_Pin GPIO_PIN_1
#define USART2_RTS_GPIO_Port GPIOD

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
