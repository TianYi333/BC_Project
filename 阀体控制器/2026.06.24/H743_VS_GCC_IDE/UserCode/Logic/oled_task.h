/**
 * @file oled_task.h
 * @brief OLED显示任务头文件
 * @author 梁伟
 * @date 2024-06-15
 */

#ifndef OLED_TASK_H
#define OLED_TASK_H

#include "cmsis_os2.h"
#include "u8g2.h"
#include "msg_queue.h"

// -------------------------- 线程配置 --------------------------
#define OLED_TASK_NAME          "OLEDTask"
#define OLED_TASK_STACK_SIZE    1024 * 4
#define OLED_TASK_PRIORITY      osPriorityNormal

#define OLED_MSG_TASK_NAME      "OLEDMsgTask"
#define OLED_MSG_TASK_STACK_SIZE    1024 * 1
#define OLED_MSG_TASK_PRIORITY      osPriorityNormal

// -------------------------- 外部变量 --------------------------
extern osThreadId_t g_oledTaskHandle;    // OLED任务句柄
extern u8g2_t g_u8g2;                    // 全局u8g2设备句柄
extern OLED_Msg oled_msg;                // OLED消息结构体

// -------------------------- 函数声明 --------------------------

/**
 * @brief 初始化OLED任务
 */
void OLED_Task_Init(void);

/**
 * @brief OLED消息任务初始化函数
 */
void OLED_Msg_Task_Init(void);

/**
 * @brief OLED任务函数
 * @param argument 任务参数（未使用）
 */
void OLED_Task_Entry(void *argument);

/**
 * @brief OLED消息任务函数
 * @param argument 任务参数（未使用）
 */
void OLED_Msg_Task_Entry(void *argument);
/**
 * @brief 发送OLED消息
 * @param msg_type 消息类型
 * @param format 消息格式字符串
 * @param ... 可变参数列表
 */
void show_oled_msg( OLED_MsgType msg_type , const char *format, ...);

#endif /* OLED_TASK_H */