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
#include "iwdg.h"
#include "msg_queue.h"
#include "net_comm_task.h"
#include "sensor_hub.h"
#include <stdio.h>

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
uint32_t start =0;
uint32_t Initialization_time = 0;

/* USER CODE END Variables */
osThreadId defaultTaskHandle;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

osThreadId IWDGTaskHandle;
osThreadId Task_InitTaskHandle;
osThreadId Udp_discover_taskHandle;
osThreadId Tcp_client_taskHandle;
osThreadId Tcp_sender_taskHandle;
osThreadId RebootTaskHandle;
osThreadId TimeSyncTaskHandle;
osThreadId SensorHubTaskHandle;

void IWDGTask(const void * argument);
void Task_InitTask(const void * argument);
void PrintTaskStackAndCount(void);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void const * argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* GetTimerTaskMemory prototype (linked to static allocation support) */
void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize );

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
   (void)xTask;
   printf("\r\n!!! STACK OVERFLOW DETECTED in task: %s !!!\r\n", pcTaskName);
   printf("Task stack high water mark (min free words): %lu\r\n",
          (unsigned long)uxTaskGetStackHighWaterMark(xTask));
   taskDISABLE_INTERRUPTS();
   for(;;);
}
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
__weak void vApplicationMallocFailedHook(void)
{
   /* vApplicationMallocFailedHook() will only be called if
   configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h. It is a hook
   function that will get called if a call to pvPortMalloc() fails.
   pvPortMalloc() is called internally by the kernel whenever a task, queue,
   timer or semaphore is created. It is also called by various parts of the
   demo application. If heap_1.c or heap_2.c are used, then the size of the
   heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
   FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
   to query the size of free heap space that remains (although it does not
   provide information on how the remaining heap might be fragmented). */
}
/* USER CODE END 5 */

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/* USER CODE BEGIN GET_TIMER_TASK_MEMORY */
static StaticTask_t xTimerTaskTCBBuffer;
static StackType_t xTimerStack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize )
{
  *ppxTimerTaskTCBBuffer = &xTimerTaskTCBBuffer;
  *ppxTimerTaskStackBuffer = &xTimerStack[0];
  *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
  /* place for user code */
}
/* USER CODE END GET_TIMER_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
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
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 2048);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  osThreadDef(IWDGTask, IWDGTask, osPriorityRealtime, 0, 128);//优先级最高，任务大小128字
  IWDGTaskHandle = osThreadCreate(osThread(IWDGTask), NULL);

  Mutex_Init();
  MsgQueue_Init();
  osDelay(3000);

  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN StartDefaultTask */
  init_net_db();
  GenerateDeviceSNFromUID();
  net_config_init();


  osThreadDef(Task_InitTask, Task_InitTask, osPriorityNormal, 0, 1024);
  Task_InitTaskHandle = osThreadCreate(osThread(Task_InitTask), NULL);
  /* Infinite loop */
  for(;;)
  {
    HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);
    osDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

void IWDGTask(const void * argument)
{
  for (;;)
  {
    /* MX_IWDG_Init() 被注释（看门狗未启用）时 hiwdg.Instance 为 NULL，
     * 直接 HAL_IWDG_Refresh 会解引用 NULL 触发 HardFault。仅在已初始化时才喂狗。 */
    if (hiwdg.Instance != NULL)
    {
      HAL_IWDG_Refresh(&hiwdg); // 循环末尾喂狗
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void Task_InitTask(const void * argument)
{
  /* FlashDB/KVDB 初始化需要较大栈空间，放在本任务（4096 字）执行，
   * 避免在 defaultTask（原 1024 字）中栈溢出破坏返回地址。 */

  osThreadDef(Udp_discover_task, Udp_discover_task, osPriorityNormal, 0, 2048);//任务大小2048字
  Udp_discover_taskHandle = osThreadCreate(osThread(Udp_discover_task), NULL);  

  osThreadDef(Tcp_client_task, Tcp_client_task, osPriorityNormal, 0, 2048);//任务大小2048字
  Tcp_client_taskHandle = osThreadCreate(osThread(Tcp_client_task), NULL);

  osThreadDef(tcp_sender_task, tcp_sender_task, osPriorityNormal, 0, 2048);//任务大小2048字
  Tcp_sender_taskHandle = osThreadCreate(osThread(tcp_sender_task), NULL);

  osThreadDef(vRebootTask, vRebootTask, osPriorityNormal, 0, 512);//任务大小512字
  RebootTaskHandle = osThreadCreate(osThread(vRebootTask), NULL);

  osThreadDef(TimeSyncTask, TimeSyncTask, osPriorityNormal, 0, 1024);//任务大小1024字
  TimeSyncTaskHandle = osThreadCreate(osThread(TimeSyncTask), NULL);

  osThreadDef(SensorHub_Task, SensorHub_Task, osPriorityNormal, 0, 2048);//8路RS485传感器采集任务
  SensorHubTaskHandle = osThreadCreate(osThread(SensorHub_Task), NULL);

  LOG_RTS("init run time: %lu ms", HAL_GetTick() - start);

  /* 临时诊断：任务创建结果与剩余堆（排查堆不足导致任务创建失败） */
  printf("[DBG] heap left after tasks: %u bytes\r\n", (unsigned)xPortGetFreeHeapSize());
  printf("[DBG] task_create: sensorhub=%s tcp_client=%s udp=%s\r\n",
         SensorHubTaskHandle  ? "OK" : "FAIL",
         Tcp_client_taskHandle ? "OK" : "FAIL",
         Udp_discover_taskHandle ? "OK" : "FAIL");
  Initialization_time = (HAL_GetTick() - start + 10000)/1000; // 预留5秒，确保系统初始化完成
  vTaskDelay(5000);
  for (;;)
  {
    PrintTaskStackAndCount();
    vTaskDelay(10000);

  }
}

void PrintTaskStackAndCount(void)
{
    // 1. 获取当前系统总任务数量
    UBaseType_t total_task_num = uxTaskGetNumberOfTasks();
    LOG_RTS("==================== System Task Info ====================");
    LOG_RTS("Total running task count: %lu", (unsigned long)total_task_num);
    LOG_RTS("----------------------------------------------------------");

    // 2. 逐个打印你创建的所有业务/网络任务最小剩余栈水位
    LOG_RTS("1 ,defaultTaskHandle         min stack left: %lu bytes", uxTaskGetStackHighWaterMark(defaultTaskHandle));
    LOG_RTS("2 ,IWDGTaskHandle            min stack left: %lu bytes", uxTaskGetStackHighWaterMark(IWDGTaskHandle));
    LOG_RTS("3 ,Task_InitTaskHandle       min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Task_InitTaskHandle));
    LOG_RTS("4 ,Udp_discover_taskHandle   min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Udp_discover_taskHandle));
    LOG_RTS("5 ,Tcp_client_taskHandle     min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Tcp_client_taskHandle));
    LOG_RTS("6 ,Tcp_sender_taskHandle     min stack left: %lu bytes", uxTaskGetStackHighWaterMark(Tcp_sender_taskHandle));
    LOG_RTS("7 ,RebootTaskHandle          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(RebootTaskHandle));
    LOG_RTS("8 ,TimeSyncTaskHandle        min stack left: %lu bytes", uxTaskGetStackHighWaterMark(TimeSyncTaskHandle));
    LOG_RTS("9 ,SensorHubTaskHandle       min stack left: %lu bytes", uxTaskGetStackHighWaterMark(SensorHubTaskHandle));

    LOG_RTS("----------------------------------------------------------");
    // 3. 打印堆内存使用水位
    size_t heap_free_now = xPortGetFreeHeapSize();//当前系统实时剩余可用堆内存字节数
    size_t heap_free_min = xPortGetMinimumEverFreeHeapSize();//堆内存出现过的最小剩余字节
    LOG_RTS("Heap Now Free: %u Bytes | Min Free Heap: %u Bytes", heap_free_now, heap_free_min);
    LOG_RTS("==========================================================\r\n");
}

/* USER CODE END Application */

