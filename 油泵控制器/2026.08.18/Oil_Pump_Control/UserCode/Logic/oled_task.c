/**
 * @file oled_task.c
 * @brief OLED显示任务实现文件
 * @author 梁伟
 * @date 2024-06-15
 */

#include "oled_task.h"
#include "oled_u8g2.h"
#include "oled_page.h"
#include "Encoder.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include <stdio.h>

// -------------------------- 全局变量 --------------------------
osThreadId_t g_oledTaskHandle; // OLED任务句柄
osThreadId_t OLED_Msg_TaskkHandle;
u8g2_t g_u8g2;                 // 全局u8g2设备句柄
OLED_Msg oled_msg;             // OLED消息结构体

// -------------------------- 函数实现 --------------------------

// -------------------------- 线程属性 --------------------------
const osThreadAttr_t oledTask_attributes = {
    .name = OLED_TASK_NAME,
    .stack_size = OLED_TASK_STACK_SIZE,
    .priority = (osPriority_t)OLED_TASK_PRIORITY,
};

const osThreadAttr_t oledMsgTask_attributes = {
    .name = OLED_MSG_TASK_NAME,
    .stack_size = OLED_MSG_TASK_STACK_SIZE,
    .priority = (osPriority_t)OLED_MSG_TASK_PRIORITY,
};

/**
 * @brief 初始化OLED任务
 */
void OLED_Task_Init(void)
{
    // 创建OLED任务
    g_oledTaskHandle = osThreadNew(OLED_Task_Entry, NULL, &oledTask_attributes);
    if (g_oledTaskHandle == NULL)
    {
        // 任务创建失败处理
        Error_Handler();
    }
}

/**
 * @brief OLED消息任务初始化函数
 */
void OLED_Msg_Task_Init(void)
{
    // 创建OLED消息任务
    OLED_Msg_TaskkHandle = osThreadNew(OLED_Msg_Task_Entry, NULL, &oledMsgTask_attributes);
}

/**
 * @brief OLED任务函数
 * @param argument 任务参数（未使用）
 */
void OLED_Task_Entry(void *argument)
{
    u8g2Init(&g_u8g2);
    show_oled_boot_page(&g_u8g2);

    extern Menu g_menu;
    extern MenuItem g_menu_items[];
    menu_init(&g_menu, g_menu_items, sizeof(g_menu_items) / sizeof(MenuItem));
    OLED_Msg_Task_Init();

    home_page_callback();
    menu_reset_idle_timer(); // 初始化全局计时

    EncoderState encoder_state;

    for (;;)
    {
        vTaskDelay(1);
        // 共用全局闲置超时判断，和二级页面计时统一
        if(menu_is_idle_timeout())
        {
            g_menu.cur_index = 0;
            g_menu.top_index = 0;
            home_page_callback();
            menu_reset_idle_timer(); // 重置全局计时
            continue;
        }
        show_oled_main_menu_page(&g_u8g2);

        // 50ms轮询读取按键
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(50)))
        {
            menu_reset_idle_timer(); // 操作统一刷新全局计时
            switch (encoder_state)
            {
            case ENCODER_CW:
                menu_handle_event(MENU_EVENT_DOWN);
                break;
            case ENCODER_CCW:
                menu_handle_event(MENU_EVENT_UP);
                break;
            case ENCODER_PUTH:
                menu_handle_event(MENU_EVENT_ENTER);
                break;
            case ENCODER_PUTH_LONG:
                menu_handle_event(MENU_EVENT_BACK);
                break;
            default:
                break;
            }
        }
    }
}

/**
 * @brief OLED消息任务函数
 * @param argument 任务参数（未使用）
 */
void OLED_Msg_Task_Entry(void *argument)
{
    oled_msg.type = OLED_MSG_NULL;
    uint32_t get_msg_time = 0;
    uint32_t show_msg_time = 0;
    for (;;)
    {
        // 等待接收OLED消息
        if (xQueueReceive(QUEUE_OLED, &oled_msg, 100))
        {
            //get_msg_time = xTaskGetTickCountFromISR();
            get_msg_time = xTaskGetTickCount();// 获取当前时间戳（任务中使用）
        }
        else
        {
            if (oled_msg.type != OLED_MSG_NULL)
            {
                //show_msg_time = xTaskGetTickCountFromISR();
                show_msg_time = xTaskGetTickCount();// 获取当前时间戳（任务中使用）
                if (show_msg_time - get_msg_time >= 4000)
                {
                    oled_msg.type = OLED_MSG_NULL;
                }
            }
        }
        vTaskDelay(1);
    }
}

/**
 * @brief 发送OLED消息
 * @param msg_type 消息类型
 * @param format 消息格式字符串
 * @param ... 可变参数列表
 */
void show_oled_msg(OLED_MsgType msg_type, const char *format, ...)
{
    OLED_Msg msg;
    msg.type = msg_type;
    va_list args;
    va_start(args, format);
    vsnprintf(msg.data, sizeof(msg.data), format, args);
    va_end(args);

    // 发送消息到OLED队列
    xQueueSend(QUEUE_OLED, &msg, portMAX_DELAY);
}