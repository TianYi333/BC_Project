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
QueueHandle_t udp_msg_queue = NULL;
QueueHandle_t xTcpTaskQueue = NULL;
QueueHandle_t xTimerReqQueue = NULL;

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
/**********************任务*************************************/
osThreadId_t udpTaskHandle;
const osThreadAttr_t udpTask_attributes = {
  .name = "udpTask",
  .stack_size = 2048 * 4,     // 足够跑 UDP + JSON
  .priority = (osPriority_t) osPriorityNormal1,  // 与MODBUS通讯任务相同比普通任务高
};

// TCP 客户端任务配置（TCP 9530）
osThreadId_t tcpConfigTaskHandle;
const osThreadAttr_t tcpConfigTask_attributes = {
  .name = "tcpConfigTask",        // 任务名，唯一不重复
  .stack_size = 2048 * 4,         // 栈大小：4K（足够TCP+JSON解析）
  .priority = (osPriority_t) osPriorityNormal1,  // 优先级同TCP任务
};

// TCP 客户端TCP 任务处理任务（从TCP任务队列取出任务信息，执行注油）
osThreadId_t TCP_task_processingTaskHandle;
const osThreadAttr_t TCP_task_processingTask_attributes = {
  .name = "TCP_task_processingTask",        // 任务名，唯一不重复
  .stack_size = 1024* 4,         // 栈大小：4K（足够TCP+JSON解析）
  .priority = (osPriority_t) osPriorityNormal1,  // 优先级同TCP任务
};

// 定时上报任务（从定时上报专用队列取出信息，执行定时上报）
osThreadId_t TimerReqSendTaskHandle;
const osThreadAttr_t TimerReqSendTask_attributes = {
  .name = "TimerReqSendTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t)osPriorityNormal1,
};

// 时间同步任务句柄
osThreadId_t TimeSyncTaskHandle;
const osThreadAttr_t TimeSyncTask_attributes = {
  .name       = "TimeSyncTask",
  .stack_size = 512 * 4,
  .priority   = osPriorityNormal1, 
};

// 测试注油任务句柄
osThreadId_t Test_Oil_FillingTaskHandle;
const osThreadAttr_t Test_Oil_FillingTask_attributes = {
  .name       = "Test_Oil_FillingTask",
  .stack_size = 1024 * 4,
  .priority   = osPriorityNormal1, 
};

/**********************互斥锁*************************************/
osMutexId_t uart_mutex;//互斥锁，保护UART资源，避免多任务打印冲突
osMutexAttr_t uart_mutex_attr = {
    .name = "uart_mutex",
    .attr_bits = osMutexRecursive, // 支持递归锁，防止printf嵌套死锁
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

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  udp_msg_queue = xQueueCreate(UDP_MSG_QUEUE_LEN, sizeof(UdpMsgTypeDef));
  xTcpTaskQueue = xQueueCreate(TCP_TASK_QUEUE_LEN, sizeof(TaskInfo_t));
  xTimerReqQueue = xQueueCreate(8, sizeof(TaskInfo_t));
  uart_mutex = osMutexNew(&uart_mutex_attr);
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
  //—————————————必须放在MX_LWIP_Init();之后———————————————————————
  init_sys_db();//void syncif_task(void *argument)任务里的已经注释掉
  init_net_db();
  net_config_init();// 读取配网参数（必须放在init_sys_db()之后）
  //——————————————————————————————————————————————————————————————
  
  OLED_Task_Init();
  MsgQueue_Init();
  start_buzz_task();
  // buzz_contral(BUZZ_LONG);
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

  udpTaskHandle = osThreadNew(udp_discover_task, NULL, &udpTask_attributes);
  tcpConfigTaskHandle = osThreadNew(tcp_client_task, NULL, &tcpConfigTask_attributes);
  TCP_task_processingTaskHandle = osThreadNew(TCP_task_processing_task,  NULL, &TCP_task_processingTask_attributes);
  TimerReqSendTaskHandle = osThreadNew(timer_req_send_task, NULL, &TimerReqSendTask_attributes);
  TimeSyncTaskHandle = osThreadNew(TimeSyncTask, NULL, &TimeSyncTask_attributes);
  Test_Oil_FillingTaskHandle = osThreadNew(test_oil_filling_task, NULL, &Test_Oil_FillingTask_attributes);
  if(TCP_task_processingTaskHandle==0)
  {
    for (;;)
    {
        vTaskDelay(2000);
    }
  }
  // osThreadNew(udp_echo_task, NULL, NULL);
  /* Infinite loop */
  for (;;)
  {
     vTaskDelay(2000);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    // 当任务栈溢出时，会自动进入这里
    // pcTaskName 变量就是出问题的任务名字！
    (void)xTask;
    while(1)
    {
        __NOP(); // 死循环，方便调试
    }
}
/* USER CODE END Application */

