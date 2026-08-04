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
#include "shell_port.h"
#include "var_init.h"
#include "flashdb.h"
#include "net_comm_task.h"
#include "iwdg.h"
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
QueueHandle_t xRebootCmdQueue = NULL;
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
osThreadId_t ETH_InitTaskHandle;
const osThreadAttr_t ETH_InitTask_attributes = {
  .name = "ETH_InitTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t udpTaskHandle;
const osThreadAttr_t udpTask_attributes = {
  .name = "udpTask",
  .stack_size = 1024 * 10,     // 足够跑 UDP + JSON
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

// 看门狗任务句柄
osThreadId_t IWDGTaskHandle;
const osThreadAttr_t IWDGTask_attributes = {
  .name       = "IWDGTask",
  .stack_size = 128 * 4,
  .priority   = osPriorityRealtime, 
};

osThreadId_t RebootTaskHandle;
const osThreadAttr_t RebootTask_attributes = {
  .name       = "RebootTask",
  .stack_size = 512 * 4,    // 栈大小1024字节，兼容Flash读写、TCP发送、字符串操作
  .priority   = osPriorityNormal1,  // 优先级
};

/**********************互斥锁*************************************/
osMutexId_t uart_mutex;//互斥锁，保护UART资源，避免多任务打印冲突
osMutexAttr_t uart_mutex_attr = {
    .name = "uart_mutex",
    .attr_bits = osMutexRecursive, // 支持递归锁，防止printf嵌套死锁
};

osMutexId_t flash_kv_mutex;
osMutexAttr_t flash_kv_mutex_attr = {
    .name = "flash_kv_mutex",
    .attr_bits = osMutexRecursive, // 开启递归，允许同一任务重复获取
};

osMutexId_t udp_pcb_mutex;
osMutexAttr_t udp_pcb_mutex_attr = {
    .name = "udp_pcb_mutex",
    .attr_bits = osMutexRecursive,
};

osMutexId_t tcp_send_mutex;
osMutexAttr_t tcp_send_mutex_attr = {
    .name = "tcp_send_mutex",
    .attr_bits = osMutexRecursive,
};

void StartUDPDiscoverTask(void *argument);
void StartTCPClientTask(void *argument);
void ETH_InitTask(void *argument);
void IWDGTask(void *argument);

uint32_t start =0;
uint32_t Initialization_time = 0;
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
  /*初始化全局变量*/
  var_init();
  fal_init();
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
  IWDGTaskHandle = osThreadNew(IWDGTask, NULL, &IWDGTask_attributes);//创建看门狗任务
  udp_msg_queue = xQueueCreate(UDP_MSG_QUEUE_LEN, sizeof(UdpMsgTypeDef));
  xTcpTaskQueue = xQueueCreate(TCP_TASK_QUEUE_LEN, sizeof(TaskInfo_t));
  xTimerReqQueue = xQueueCreate(8, sizeof(TaskInfo_t));
  xRebootCmdQueue = xQueueCreate(1, sizeof(RebootCmd_t));
  uart_mutex = osMutexNew(&uart_mutex_attr);
  flash_kv_mutex = osMutexNew(&flash_kv_mutex_attr);
  udp_pcb_mutex = osMutexNew(&udp_pcb_mutex_attr);
  tcp_send_mutex = osMutexNew(&tcp_send_mutex_attr);
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
  GenerateDeviceSNFromUID();
  start = HAL_GetTick();
  LOG("init_net_db total start time: %lu ms", start);
  //fdb_kv_del(&net_kvdb, KV_KEY_REBOOT_INFO);// 测试用，清除掉电存储信息
  //—————————————必须放在MX_LWIP_Init();之后———————————————————————
  init_sys_db();//void syncif_task(void *argument)任务里的已经注释掉
  LOG("init_sys_db run time: %lu ms", HAL_GetTick() - start);
  init_net_db();
  LOG("init_net_db total run time: %lu ms", HAL_GetTick() - start);
  init_reboot_db();// 初始化掉电存储数据库，读取上次重启原因
  LOG(" init_reboot_db total run time: %lu ms", HAL_GetTick() - start);
  net_config_init();// 读取配网参数（必须放在init_sys_db()之后）
  //——————————————————————————————————————————————————————————————
  OLED_Task_Init();
  MsgQueue_Init();
  start_buzz_task();
	userShellInit();			//初始化配置LetterShell
  buzz_contral(BUZZ_LONG);
  fdcan1.rx_Filter_Init();
  fdcan1.rx_Interrupt_Init();
  fdcan1.start();
  fdcan2.rx_Filter_Init();
  fdcan2.rx_Interrupt_Init();
  fdcan2.start();
 

  start_syncif_task();
  vTaskDelay(1000);
  buzz_contral(BUZZ_SHORT);
  start_mbif_task();
  vTaskDelay(1000);
  buzz_contral(BUZZ_SHORT);
  start_injector_task();
  ETH_InitTaskHandle = osThreadNew(ETH_InitTask, NULL, &ETH_InitTask_attributes);
  /* Infinite loop */
  for (;;)
  {
     vTaskDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void PrintTaskStackAndCount(void)
{
    // 1. 获取当前系统总任务数量
    UBaseType_t total_task_num = uxTaskGetNumberOfTasks();
    LOG("==================== System Task Info ====================");
    LOG("Total running task count: %lu", (unsigned long)total_task_num);
    LOG("----------------------------------------------------------");

    // 2. 逐个打印你创建的所有业务/网络任务最小剩余栈水位
    LOG("1 ,IWDGTask                  min stack left: %u bytes", uxTaskGetStackHighWaterMark(IWDGTaskHandle));
    LOG("2 ,StartDefaultTask          min stack left: %u bytes", uxTaskGetStackHighWaterMark(defaultTaskHandle));
    LOG("3 ,OLED_Task_Entry           min stack left: %u bytes", uxTaskGetStackHighWaterMark(g_oledTaskHandle));
    LOG("4 ,OLED_Msg_Task_Entry       min stack left: %u bytes", uxTaskGetStackHighWaterMark(OLED_Msg_TaskkHandle));
    LOG("5 ,buzz_task                 min stack left: %u bytes", uxTaskGetStackHighWaterMark(buzz_task_handler));
    LOG("6 ,syncif_task               min stack left: %u bytes", uxTaskGetStackHighWaterMark(syncif_task_handler));
    LOG("7 ,syncif_task_cycle         min stack left: %u bytes", uxTaskGetStackHighWaterMark(syncif_task_handler_cycle));
    LOG("8 ,mbif_task                 min stack left: %u bytes", uxTaskGetStackHighWaterMark(mbif_task_handler));
    LOG("9 ,injector_task             min stack left: %u bytes", uxTaskGetStackHighWaterMark(injector_task_handler));
    LOG("10 ,ETH_InitTask             min stack left: %u bytes", uxTaskGetStackHighWaterMark(ETH_InitTaskHandle));
    LOG("11,udp_discover_task         min stack left: %u bytes", uxTaskGetStackHighWaterMark(udpTaskHandle));
    LOG("12,tcp_client_task           min stack left: %u bytes", uxTaskGetStackHighWaterMark(tcpConfigTaskHandle));
    LOG("13,TCP_task_processing_task  min stack left: %u bytes", uxTaskGetStackHighWaterMark(TCP_task_processingTaskHandle));
    LOG("14,TimerReqSendTask          min stack left: %u bytes", uxTaskGetStackHighWaterMark(TimerReqSendTaskHandle));
    LOG("15,TimeSyncTask              min stack left: %u bytes", uxTaskGetStackHighWaterMark(TimeSyncTaskHandle));
    LOG("16,RebootTask                min stack left: %u bytes", uxTaskGetStackHighWaterMark(RebootTaskHandle));
    // LOG("xxx_task                min stack left: %u bytes", uxTaskGetStackHighWaterMark(xxxTaskHandle));

    LOG("----------------------------------------------------------");
    // 3. 打印堆内存使用水位
    size_t heap_free_now = xPortGetFreeHeapSize();//当前系统实时剩余可用堆内存字节数
    size_t heap_free_min = xPortGetMinimumEverFreeHeapSize();//堆内存出现过的最小剩余字节
    LOG("Heap Now Free: %u Bytes | Min Free Heap: %u Bytes", heap_free_now, heap_free_min);
    LOG("==========================================================\r\n");
}

void ETH_InitTask(void *argument)
{
  
  //—————————————必须放在MX_LWIP_Init();之后———————————————————————
  // init_net_db();
  //net_config_init();// 读取配网参数（必须放在init_sys_db()之后）
  //——————————————————————————————————————————————————————————————
  udpTaskHandle = osThreadNew(udp_discover_task, NULL, &udpTask_attributes);
  tcpConfigTaskHandle = osThreadNew(tcp_client_task, NULL, &tcpConfigTask_attributes);
  TCP_task_processingTaskHandle = osThreadNew(TCP_task_processing_task,  NULL, &TCP_task_processingTask_attributes);
  TimerReqSendTaskHandle = osThreadNew(timer_req_send_task, NULL, &TimerReqSendTask_attributes);
  TimeSyncTaskHandle = osThreadNew(TimeSyncTask, NULL, &TimeSyncTask_attributes);
  RebootTaskHandle = osThreadNew(vRebootTask, NULL, &RebootTask_attributes);
  // Test_Oil_FillingTaskHandle = osThreadNew(test_oil_filling_task, NULL, &Test_Oil_FillingTask_attributes);
  // if(Test_Oil_FillingTaskHandle==0&&TimeSyncTaskHandle==0&&TimerReqSendTaskHandle==0&&TCP_task_processingTaskHandle==0&&tcpConfigTaskHandle==0&&udpTaskHandle==0)
  // {
  //   for (;;)
  //   {
  //       vTaskDelay(2000);
  //   }
  // }
  LOG("init run time: %lu ms", HAL_GetTick() - start);
  Initialization_time = (HAL_GetTick() - start + 10000)/1000; // 预留5秒，确保系统初始化完成
  // osThreadNew(udp_echo_task, NULL, NULL);
  vTaskDelay(5000);
  for (;;)
  {
    PrintTaskStackAndCount();
    vTaskDelay(50000);

  }
}

void IWDGTask(void *argument)
{
  for (;;)
  {
    HAL_IWDG_Refresh(&hiwdg1); // 循环末尾喂狗
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}


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

