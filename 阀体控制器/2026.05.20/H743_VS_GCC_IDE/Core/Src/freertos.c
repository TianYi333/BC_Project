/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
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

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "u8g2.h"
#include "oled_u8g2.h"
#include "sht40.h"
#include "rtc_clock.h"
#include "stdio.h"
#include "string.h"
#include "oled_page.h"
#include "oled_task.h"
#include "bsp_fdcan.h"
#include "buzzer.h"
#include "mbif.h"
#include "running_logic.h"
#include "syncif.h"
#include "net_comm_task.h"
/* USER CODE END Includes */
#include "flashdb.h"// FlashDB 头文件
#include "fal.h"
/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 4096 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
osThreadId_t udpTaskHandle;
const osThreadAttr_t udpTask_attributes = {
  .name = "udpTask",
  .stack_size = 1024 * 4,     // 足够跑 UDP + JSON
  .priority = (osPriority_t) osPriorityAboveNormal,  // 比普通任务高
};

osThreadId_t tcpTaskHandle;
const osThreadAttr_t tcpTask_attributes = {
  .name = "tcpTask",
  .stack_size = 1024 * 4,     // TCP + JSON 必须足够栈
  .priority = (osPriority_t) osPriorityAboveNormal,
};

// TCP 配网服务端任务配置（TCP 9530）
osThreadId_t tcpConfigTaskHandle;
const osThreadAttr_t tcpConfigTask_attributes = {
  .name = "tcpConfigTask",        // 任务名，唯一不重复
  .stack_size = 1024 * 4,         // 栈大小：4K（足够TCP+JSON解析）
  .priority = (osPriority_t) osPriorityAboveNormal,  // 优先级同TCP任务
};


void StartUDPDiscoverTask(void *argument);
void StartTCPClientTask(void *argument);

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* USER CODE BEGIN PREPOSTSLEEP */
__weak void PreSleepProcessing(uint32_t ulExpectedIdleTime)
{
  /* place for user code */
}

__weak void PostSleepProcessing(uint32_t ulExpectedIdleTime)
{
  /* place for user code */
}
/* USER CODE END PREPOSTSLEEP */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
	init_sys_db();//void syncif_task(void *argument)任务里的已经注释掉
  // 读取配网参数（必须放在init_sys_db()之后）
  net_config_init();
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  
  udpTaskHandle = osThreadNew(udp_discover_task, NULL, &udpTask_attributes);
  tcpConfigTaskHandle = osThreadNew(tcp_config_server_task, NULL, &tcpConfigTask_attributes);
  tcpTaskHandle = osThreadNew(tcp_client_task, NULL, &tcpTask_attributes);
  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */
 
  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief  Function implementing the defaultTask thread.
 * @param  argument: Not used
 * @retval None
 */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN StartDefaultTask */
  OLED_Task_Init();
  MsgQueue_Init();
  start_buzz_task();

  buzz_contral(BUZZ_LONG);
  fdcan1.rx_Filter_Init();
  fdcan1.rx_Interrupt_Init();
  fdcan1.start();
  fdcan2.rx_Filter_Init();
  fdcan2.rx_Interrupt_Init();
  fdcan2.start();
  /**
   * 初始化系统状态，阀体归零
   */
  init_sys_status();

  start_syncif_task();
  start_mbif_task();

  start_injector_task();

  /* Infinite loop */
  for (;;)
  {
     vTaskDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void StartUDPDiscoverTask(void *argument)
{
  udp_discover_task(argument);
}

void StartTCPClientTask(void *argument)
{
  tcp_client_task(argument);
}
/* USER CODE END Application */

