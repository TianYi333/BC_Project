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
#include "project_config.h"
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
QueueHandle_t udp_msg_queue = NULL;
QueueHandle_t xTcpTaskQueue = NULL;
QueueHandle_t xTimerReqQueue = NULL;
QueueHandle_t xRebootCmdQueue = NULL;
QueueHandle_t tcp_send_queue = NULL;
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
  .stack_size = 1024 * 4,
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
  .stack_size = 1024 * 10,         // 栈大小：4K（足够TCP+JSON解析）
  .priority = (osPriority_t) osPriorityNormal1,  // 优先级同TCP任务
};

// TCP 客户端TCP 任务处理任务（从TCP任务队列取出任务信息，执行注油）
osThreadId_t TCP_task_processingTaskHandle;
const osThreadAttr_t TCP_task_processingTask_attributes = {
  .name = "TCP_task_processingTask",        // 任务名，唯一不重复
  .stack_size = 1024* 6,         // 栈大小：4K（足够TCP+JSON解析）
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
  .stack_size = 1024 * 4,
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
  .stack_size = 256 * 4,
  .priority   = osPriorityRealtime7,   // =55, 为合法最高优先级(0..55), 高于EthIf的osPriorityRealtime(48)，确保频繁收发时也能抢占喂狗
};

osThreadId_t RebootTaskHandle;
const osThreadAttr_t RebootTask_attributes = {
  .name       = "RebootTask",
  .stack_size = 1024 * 4,    // 栈大小1024字节，兼容Flash读写、TCP发送、字符串操作
  .priority   = osPriorityNormal1,  // 优先级
};

osThreadId_t TcpSenderTaskHandle;
const osThreadAttr_t TcpSenderTask_attributes = {
    .name       = "TcpSender",
    .stack_size = 1024 * 8,                // 4096 字节，足够容纳 2048 字节栈缓冲区
    .priority   = osPriorityNormal,       // 可根据系统负载调整
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
void PrintTaskStackAndCount_2(void);
static void TaskMonitor_InitHandle(void);

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
  tcp_send_queue = xQueueCreate(32, sizeof(tcp_send_msg_t));
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
  LOG_RTS("init_net_db total start time: %lu ms", start);
  //fdb_kv_del(&net_kvdb, KV_KEY_REBOOT_INFO);// 测试用，清除掉电存储信息
  //—————————————必须放在MX_LWIP_Init();之后———————————————————————
  init_sys_db();//void syncif_task(void *argument)任务里的已经注释掉
  LOG_RTS("init_sys_db run time: %lu ms", HAL_GetTick() - start);
  init_net_db();
  LOG_RTS("init_net_db total run time: %lu ms", HAL_GetTick() - start);
  init_reboot_db();// 初始化掉电存储数据库，读取上次重启原因
  LOG_RTS("init_reboot_db total run time: %lu ms", HAL_GetTick() - start);
  net_config_init();// 读取配网参数（必须放在init_sys_db()之后）
  //——————————————————————————————————————————————————————————————
  OLED_Task_Init();
  MsgQueue_Init();
  start_buzz_task();
	//userShellInit();			//初始化配置LetterShell
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
    LOG_RTS("==================== System Task Info ====================");
    LOG_RTS("Total running task count: %lu", (unsigned long)total_task_num);
    LOG_RTS("----------------------------------------------------------");

    // 2. 逐个打印你创建的所有业务/网络任务最小剩余栈水位
    LOG_RTS("1 ,IWDGTask                  min stack left: %lu bytes", uxTaskGetStackHighWaterMark(IWDGTaskHandle));
    LOG_RTS("2 ,StartDefaultTask          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(defaultTaskHandle));
    LOG_RTS("3 ,OLED_Task_Entry           min stack left: %lu bytes", uxTaskGetStackHighWaterMark(g_oledTaskHandle));
    LOG_RTS("4 ,OLED_Msg_Task_Entry       min stack left: %lu bytes", uxTaskGetStackHighWaterMark(OLED_Msg_TaskkHandle));
    LOG_RTS("5 ,buzz_task                 min stack left: %lu bytes", uxTaskGetStackHighWaterMark(buzz_task_handler));
    LOG_RTS("6 ,syncif_task               min stack left: %lu bytes", uxTaskGetStackHighWaterMark(syncif_task_handler));
    LOG_RTS("7 ,syncif_task_cycle         min stack left: %lu bytes", uxTaskGetStackHighWaterMark(syncif_task_handler_cycle));
    LOG_RTS("8 ,mbif_task                 min stack left: %lu bytes", uxTaskGetStackHighWaterMark(mbif_task_handler));
    LOG_RTS("9 ,injector_task             min stack left: %lu bytes", uxTaskGetStackHighWaterMark(injector_task_handler));
    LOG_RTS("10,ETH_InitTask              min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ETH_InitTaskHandle));
    LOG_RTS("11,udp_discover_task         min stack left: %lu bytes", uxTaskGetStackHighWaterMark(udpTaskHandle));
    LOG_RTS("12,tcp_client_task           min stack left: %lu bytes", uxTaskGetStackHighWaterMark(tcpConfigTaskHandle));
    LOG_RTS("13,TCP_task_processing_task  min stack left: %lu bytes", uxTaskGetStackHighWaterMark(TCP_task_processingTaskHandle));
    LOG_RTS("14,TimerReqSendTask          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(TimerReqSendTaskHandle));
    LOG_RTS("15,TimeSyncTask              min stack left: %lu bytes", uxTaskGetStackHighWaterMark(TimeSyncTaskHandle));
    LOG_RTS("16,RebootTask                min stack left: %lu bytes", uxTaskGetStackHighWaterMark(RebootTaskHandle));
    LOG_RTS("17,TcpSenderTask             min stack left: %lu bytes", uxTaskGetStackHighWaterMark(TcpSenderTaskHandle));
    LOG_RTS("18,ethernet_link_thread      min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ethernet_link_threadHandle));
    LOG_RTS("19,ethernetif_input          min stack left: %lu bytes", uxTaskGetStackHighWaterMark(ethernetif_inputHandle));
    // LOG_RTS("xxx_task                min stack left: %u bytes", uxTaskGetStackHighWaterMark(xxxTaskHandle));
    // LOG_RTS("xxx_task                min stack left: %u bytes", uxTaskGetStackHighWaterMark(xxxTaskHandle));

    LOG_RTS("----------------------------------------------------------");
    // 3. 打印堆内存使用水位
    size_t heap_free_now = xPortGetFreeHeapSize();//当前系统实时剩余可用堆内存字节数
    size_t heap_free_min = xPortGetMinimumEverFreeHeapSize();//堆内存出现过的最小剩余字节
    LOG_RTS("Heap Now Free: %u Bytes | Min Free Heap: %u Bytes", heap_free_now, heap_free_min);
    LOG_RTS("==========================================================\r\n");
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
  TcpSenderTaskHandle = osThreadNew(tcp_sender_task, NULL, &TcpSenderTask_attributes);
  // Test_Oil_FillingTaskHandle = osThreadNew(test_oil_filling_task, NULL, &Test_Oil_FillingTask_attributes);
  // if(Test_Oil_FillingTaskHandle==0&&TimeSyncTaskHandle==0&&TimerReqSendTaskHandle==0&&TCP_task_processingTaskHandle==0&&tcpConfigTaskHandle==0&&udpTaskHandle==0)
  // {
  //   for (;;)
  //   {
  //       vTaskDelay(2000);
  //   }
  // }
  LOG_RTS("init run time: %lu ms", HAL_GetTick() - start);
  Initialization_time = (HAL_GetTick() - start + 10000)/1000; // 预留5秒，确保系统初始化完成
  // osThreadNew(udp_echo_task, NULL, NULL);
  vTaskDelay(5000);
  TaskMonitor_InitHandle();
  for (;;)
  {
    //PrintTaskStackAndCount();
    PrintTaskStackAndCount_2();
    vTaskDelay(50000);

  }
}

void IWDGTask(void *argument)
{
  for (;;)
  {
    HAL_IWDG_Refresh(&hiwdg1); // 循环末尾喂狗
    vTaskDelay(pdMS_TO_TICKS(100));
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

/* 栈告警阈值：单位 words，剩余小于该值触发告警，建议 80~120 */
#define TASK_STACK_ALERT_WORDS      100U
/* 连续多少次检测栈持续下降才报泄漏警告，避免瞬时波动 */
#define STACK_DROP_DETECT_CNT       5U

/* 保存每个任务上一次的栈水位(words)，用于检测持续下降 */
typedef struct
{
    TaskHandle_t handle;
    const char*  task_name;
    UBaseType_t  last_watermark;
    uint8_t      drop_cnt;     /* 连续下降计数 */
}TASK_STACK_MONITOR_T;

/* 任务监控列表，和你PrintTaskStackAndCount里面任务一一对应 */
static TASK_STACK_MONITOR_T g_task_monitor_list[] =
{
    {NULL, "IWDGTask",                     0,0},
    {NULL, "StartDefaultTask",             0,0},
    {NULL, "OLED_Task_Entry",              0,0},
    {NULL, "OLED_Msg_Task_Entry",          0,0},
    {NULL, "buzz_task",                    0,0},
    {NULL, "syncif_task",                  0,0},
    {NULL, "syncif_task_cycle",            0,0},
    {NULL, "mbif_task",                    0,0},
    {NULL, "injector_task",                0,0},
    {NULL, "ETH_InitTask",                 0,0},
    {NULL, "udp_discover_task",            0,0},
    {NULL, "tcp_client_task",              0,0},
    {NULL, "TCP_task_processing_task",     0,0},
    {NULL, "TimerReqSendTask",             0,0},
    {NULL, "TimeSyncTask",                 0,0},
    {NULL, "RebootTask",                   0,0},
    {NULL, "TcpSenderTask",                0,0},
    {NULL, "ethernet_link_thread",         0,0},
    {NULL, "ethernetif_input",             0,0},
};
#define TASK_MONITOR_NUM    (sizeof(g_task_monitor_list)/sizeof(TASK_STACK_MONITOR_T))
// 在所有osThreadNew执行完之后调用，回填句柄
static void TaskMonitor_InitHandle(void)
{
    g_task_monitor_list[0].handle  = IWDGTaskHandle;
    g_task_monitor_list[1].handle  = defaultTaskHandle;
    g_task_monitor_list[2].handle  = g_oledTaskHandle;
    g_task_monitor_list[3].handle  = OLED_Msg_TaskkHandle;
    g_task_monitor_list[4].handle  = buzz_task_handler;
    g_task_monitor_list[5].handle  = syncif_task_handler;
    g_task_monitor_list[6].handle  = syncif_task_handler_cycle;
    g_task_monitor_list[7].handle  = mbif_task_handler;
    g_task_monitor_list[8].handle  = injector_task_handler;
    g_task_monitor_list[9].handle  = ETH_InitTaskHandle;
    g_task_monitor_list[10].handle = udpTaskHandle;
    g_task_monitor_list[11].handle = tcpConfigTaskHandle;
    g_task_monitor_list[12].handle = TCP_task_processingTaskHandle;
    g_task_monitor_list[13].handle = TimerReqSendTaskHandle;
    g_task_monitor_list[14].handle = TimeSyncTaskHandle;
    g_task_monitor_list[15].handle = RebootTaskHandle;
    g_task_monitor_list[16].handle = TcpSenderTaskHandle;
    g_task_monitor_list[17].handle = ethernet_link_threadHandle;
    g_task_monitor_list[18].handle = ethernetif_inputHandle;
}
#define TASK_MONITOR_NUM    (sizeof(g_task_monitor_list)/sizeof(TASK_STACK_MONITOR_T))

void PrintTaskStackAndCount_2(void)
{
    UBaseType_t total_task_num = uxTaskGetNumberOfTasks();
    LOG_RTS("==================== System Task Info ====================");
    LOG_RTS("Total running task count: %lu", (unsigned long)total_task_num);
    LOG_RTS("----------------------------------------------------------");

    for(uint32_t i = 0; i < TASK_MONITOR_NUM; i++)
    {
        TASK_STACK_MONITOR_T *p_item = &g_task_monitor_list[i];
        if(p_item->handle == NULL)
        {
            LOG_RTS("%-25s Handle=NULL", p_item->task_name);
            continue;
        }

        UBaseType_t curr_water = uxTaskGetStackHighWaterMark(p_item->handle);
        uint32_t curr_byte = curr_water * sizeof(StackType_t);

        // 如果有下降计数，就打印出来
        if(p_item->drop_cnt > 0U)
        {
            LOG_RTS("%-25s min stack left: %lu words / %lu bytes, drop_cnt:%lu",
                p_item->task_name, curr_water, curr_byte, (unsigned long)p_item->drop_cnt);
        }
        else
        {
            LOG_RTS("%-25s min stack left: %lu words / %lu bytes",
                p_item->task_name, curr_water, curr_byte);
        }

        /* ==========阈值告警逻辑========== */
        if(curr_water < TASK_STACK_ALERT_WORDS)
        {
            LOG_RTS("[WARN] STACK LOW! Task:%s remain words:%lu", p_item->task_name, curr_water);
        }

        /* 判断栈是否持续变小（栈泄漏趋势检测） */
        if(p_item->last_watermark != 0U)
        {
            if(curr_water < p_item->last_watermark)
            {
                p_item->drop_cnt ++;
                if(p_item->drop_cnt >= STACK_DROP_DETECT_CNT)
                {
                    LOG_RTS("[CRITICAL] STACK CONTINUE DROP! Task:%s last:%lu curr:%lu, drop_cnt:%lu",
                        p_item->task_name, p_item->last_watermark, curr_water, (unsigned long)p_item->drop_cnt);
                }
            }
            else
            {
                /* 不再下降，清零计数 */
                p_item->drop_cnt = 0U;
            }
        }
        p_item->last_watermark = curr_water;
    }

    LOG_RTS("----------------------------------------------------------");
    size_t heap_free_now = xPortGetFreeHeapSize();
    size_t heap_free_min = xPortGetMinimumEverFreeHeapSize();
    LOG_RTS("Heap Now Free: %u Bytes | Min Free Heap: %u Bytes", heap_free_now, heap_free_min);

    /* 堆内存简单告警，可自定义阈值 */
#define HEAP_ALERT_THRESHOLD    (4*1024U)
    if(heap_free_min < HEAP_ALERT_THRESHOLD)
    {
        LOG_RTS("[WARN] HEAP LOW WARNING! min free heap:%u", heap_free_min);
    }

    LOG_RTS("==========================================================\r\n");
}

// 自行实现basename，裸机STM32可用，替代libgen.h的basename
const char *my_basename(const char *path)
{
    const char *p = path;
    const char *last = path;
    for (; *p != '\0'; p++)
    {
        if(*p == '/' || *p == '\\')
        {
            last = p + 1;
        }
    }
    return last;
}
/* USER CODE END Application */

