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
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "u8g2.h"
#include "oled_u8g2.h"
#include "sht40.h"
#include "rtc_clock.h"
#include "stdio.h"
#include "string.h"
#include "oled_page.h"
#include "oled_task.h"
#include "buzzer.h"
#include "mbif.h"
#include "syncif.h"
#include "shell_port.h"
#include "var_init.h"
#include "flashdb.h"
#include "net_comm_task.h"
#include "iwdg.h"
#include "ADC_collect.h"
#include "main_logic.h"
#include "pid_ctrl.h"
#include "modbus_master.h"
#include "step_motor.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
extern osThreadId_t ethernet_link_threadHandle;
extern osThreadId_t ethernetif_inputHandle;
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
/**********************任务*************************************/
osThreadId_t Task_InitTaskHandle;
const osThreadAttr_t Task_InitTask_attributes = {
  .name = "Task_InitTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t Udp_discover_taskHandle;
const osThreadAttr_t Udp_discover_task_attributes = {
  .name = "Udp_discover_task",
  .stack_size = 1024 * 10,     // 足够跑 UDP + JSON
  .priority = (osPriority_t) osPriorityNormal1,  // 与MODBUS通讯任务相同比普通任务高
};

// TCP 客户端任务配置（TCP 9530）
osThreadId_t Tcp_client_taskHandle;
const osThreadAttr_t Tcp_client_task_attributes = {
  .name = "Tcp_client_task",        // 任务名，唯一不重复
  .stack_size = 2048 * 4,         // 栈大小：4K（足够TCP+JSON解析）
  .priority = (osPriority_t) osPriorityNormal1,  // 优先级同TCP任务
};

// 时间同步任务句柄
osThreadId_t TimeSyncTaskHandle;
const osThreadAttr_t TimeSyncTask_attributes = {
  .name       = "TimeSyncTask",
  .stack_size = 1024 * 4,
  .priority   = osPriorityNormal1, 
};

// 看门狗任务句柄
osThreadId_t IWDGTaskHandle;
const osThreadAttr_t IWDGTask_attributes = {
  .name       = "IWDGTask",
  .stack_size = 128 * 4,
  .priority   = osPriorityRealtime7,   // =55, 为合法最高优先级(0..55), 高于EthIf的osPriorityRealtime(48)，确保频繁收发时也能抢占喂狗
};

osThreadId_t RebootTaskHandle;
const osThreadAttr_t RebootTask_attributes = {
  .name       = "RebootTask",
  .stack_size = 512 * 4,    // 栈大小1024字节，兼容Flash读写、TCP发送、字符串操作
  .priority   = osPriorityNormal1,  // 优先级
};

osThreadId_t TcpSenderTaskHandle;
const osThreadAttr_t TcpSenderTask_attributes = {
    .name       = "TcpSender",
    .stack_size = 1024 * 8,                // 4096 字节，足够容纳 2048 字节栈缓冲区
    .priority   = osPriorityNormal,       // 可根据系统负载调整
};

osThreadId_t ADCTaskHandle;
const osThreadAttr_t ADCTask_attributes = {
  .name       = "ADCTask",
  .stack_size = 1024 * 4,
  .priority   = osPriorityNormal1,
};

osThreadId_t MainLogicTaskHandle;//主逻辑任务句柄
const osThreadAttr_t MainLogicTask_attributes = {
    .name = "MainLogicTask",
    .stack_size = 1024*4,
    .priority = osPriorityNormal1
};

// 压力闭环调速任务
osThreadId_t PressureControlTaskHandle;
const osThreadAttr_t PressureControlTask_attributes = {
    .name = "PressureControlTask",
    .stack_size = 1024 * 4,
    .priority = osPriorityAboveNormal7    // 高于主逻辑
};

// Modbus主机轮询任务
osThreadId_t ModbusMasterTaskHandle;
const osThreadAttr_t ModbusMasterTask_attributes = {
    .name = "ModbusMasterTask",
    .stack_size = 1024 * 4,
    .priority = osPriorityNormal1         // 和主逻辑同级
};

// 步进电机任务
osThreadId_t Step_speedTaskHandle;
const osThreadAttr_t Step_speedTask_attributes = {
    .name = "Step_speedTask",
    .stack_size = 512 * 4,
    .priority = osPriorityAboveNormal7         // 高于主逻辑
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
    .attr_bits = osMutexRecursive,// 开启递归，允许同一任务重复获取
};

osMutexId_t sys_status_mutex;
const osMutexAttr_t sys_status_mutex_attr = {
    .name = "sys_status_mutex",
    .attr_bits = osMutexRecursive,        // 优先级继承，确保高优先级任务先获取锁
};

void StartUDPDiscoverTask(void *argument);
void StartTCPClientTask(void *argument);
void Task_InitTask(void *argument);
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
  uart_mutex = osMutexNew(&uart_mutex_attr);
  flash_kv_mutex = osMutexNew(&flash_kv_mutex_attr);
  udp_pcb_mutex = osMutexNew(&udp_pcb_mutex_attr);
  tcp_send_mutex = osMutexNew(&tcp_send_mutex_attr);
  sys_status_mutex = osMutexNew(&sys_status_mutex_attr);
  MsgQueue_Init();
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
  //—————————————必须放在MX_LWIP_Init();之后———————————————————————
  init_sys_db();//void syncif_task(void *argument)任务里的已经注释掉
  LOG("init_sys_db run time: %lu ms", HAL_GetTick() - start);
  init_net_db();
  LOG("init_net_db total run time: %lu ms", HAL_GetTick() - start);
   init_reboot_db();// 初始化掉电存储数据库，读取上次重启原因
  LOG("init_reboot_db total run time: %lu ms", HAL_GetTick() - start);
  net_config_init();// 读取配网参数（必须放在init_sys_db()之后）
  //——————————————————————————————————————————————————————————————
  OLED_Task_Init();
  //start_buzz_task(); 
  buzz_contral(BUZZ_LONG);
 
  start_syncif_task();
  vTaskDelay(1000);
  buzz_contral(BUZZ_SHORT);
  start_mbif_task();
  vTaskDelay(1000);
  buzz_contral(BUZZ_SHORT);
  Task_InitTaskHandle = osThreadNew(Task_InitTask, NULL, &Task_InitTask_attributes);
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
    LOG("1 ,IWDGTask                  min stack left: %lu bytes", uxTaskGetStackHighWaterMark(IWDGTaskHandle));
    LOG("2 ,StartDefaultTask          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(defaultTaskHandle));
    LOG("3 ,OLED_Task_Entry           min stack left: %lu bytes", uxTaskGetStackHighWaterMark(g_oledTaskHandle));
    LOG("4 ,OLED_Msg_Task_Entry       min stack left: %lu bytes", uxTaskGetStackHighWaterMark(OLED_Msg_TaskkHandle));
    LOG("5 ,Buzz_task                 min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Buzz_task_handler));
    LOG("6 ,Syncif_task               min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Syncif_task_handler));
    LOG("7 ,MBif_task                 min stack left: %lu bytes", uxTaskGetStackHighWaterMark(MBif_task_handler));
    LOG("8 ,MainLogicTask             min stack left: %lu bytes", uxTaskGetStackHighWaterMark(MainLogicTaskHandle));
    LOG("9 ,Task_InitTask             min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Task_InitTaskHandle));
    LOG("10,Udp_discover_task         min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Udp_discover_taskHandle));
    LOG("11,Tcp_client_task           min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Tcp_client_taskHandle));
    LOG("14,TimeSyncTask              min stack left: %lu bytes", uxTaskGetStackHighWaterMark(TimeSyncTaskHandle));
    LOG("15,ADCTask                   min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ADCTaskHandle));
    LOG("16,PressureControlTask       min stack left: %lu bytes", uxTaskGetStackHighWaterMark(PressureControlTaskHandle));
    LOG("17,ModbusMasterTask          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ModbusMasterTaskHandle));
    LOG("18,ethernet_link_thread      min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ethernet_link_threadHandle));
    LOG("19,ethernetif_input          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ethernetif_inputHandle));
    // LOG("xxx_task                min stack left: %u bytes", uxTaskGetStackHighWaterMark(xxxTaskHandle));

    LOG("----------------------------------------------------------");
    // 3. 打印堆内存使用水位
    size_t heap_free_now = xPortGetFreeHeapSize();//当前系统实时剩余可用堆内存字节数
    size_t heap_free_min = xPortGetMinimumEverFreeHeapSize();//堆内存出现过的最小剩余字节
    LOG("Heap Now Free: %u Bytes | Min Free Heap: %u Bytes", heap_free_now, heap_free_min);
    LOG("==========================================================\r\n");
}

void Task_InitTask(void *argument)
{
  MainLogicTaskHandle = osThreadNew(MainLogicTask, NULL, &MainLogicTask_attributes);
  PressureControlTaskHandle = osThreadNew(PressureControlTask, NULL, &PressureControlTask_attributes);
  ModbusMasterTaskHandle = osThreadNew(ModbusMasterTask, NULL, &ModbusMasterTask_attributes);
  Udp_discover_taskHandle = osThreadNew(Udp_discover_task, NULL, &Udp_discover_task_attributes);
  Tcp_client_taskHandle = osThreadNew(Tcp_client_task, NULL, &Tcp_client_task_attributes);
  TimeSyncTaskHandle = osThreadNew(TimeSyncTask, NULL, &TimeSyncTask_attributes);
  RebootTaskHandle = osThreadNew(vRebootTask, NULL, &RebootTask_attributes);
  TcpSenderTaskHandle = osThreadNew(tcp_sender_task, NULL, &TcpSenderTask_attributes);
  ADCTaskHandle = osThreadNew(ADCTask, NULL, &ADCTask_attributes);
  #if defined(MOTOR_DRIVER_PULSE_STEPPER)
    Step_speedTaskHandle = osThreadNew(step_speed_ctrl_task, NULL, &Step_speedTask_attributes);
  #endif
  LOG("init run time: %lu ms", HAL_GetTick() - start);
  Initialization_time = (HAL_GetTick() - start + 10000)/1000; // 预留5秒，确保系统初始化完成
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

