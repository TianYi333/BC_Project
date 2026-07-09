/**
 * @file msg_queue.c
 *  @brief 消息队列实现文件
 *  @author 梁伟
 *  @date 2024-06-15
 */

#include "msg_queue.h"

/**
 * 蜂鸣器操作消息
 */
QueueHandle_t QUEUE_BUZZ;
/**
 * oled错误消息
 */
QueueHandle_t QUEUE_OLED;
/**
 * 配置修改消息
 */
QueueHandle_t QUEUE_CONN;
/**
 * 键盘操作消息
 */
QueueHandle_t QUEUE_KEY;
/**
 * 金属接近传感器
 */
QueueHandle_t QUEUE_MET_SEN;

/**
 * CAN1其它消息
 */
QueueHandle_t msgQueue_ID_CAN1;
/**
 * CAN2其它消息
 */
QueueHandle_t msgQueue_ID_CAN2;

/**
 * @brief 初始化消息队列
 */
void MsgQueue_Init(void) {
  QUEUE_BUZZ = xQueueCreate(1, sizeof(uint8_t));
	QUEUE_OLED = xQueueCreate(1, sizeof(OLED_Msg));
	QUEUE_KEY = xQueueCreate(1, sizeof(uint8_t));
	QUEUE_CONN = xQueueCreate(1, sizeof(_MB_REG));
	QUEUE_MET_SEN = xQueueCreate(1, sizeof(uint8_t));
	msgQueue_ID_CAN1 = xQueueCreate(4, sizeof(_CANMSG));
	msgQueue_ID_CAN2 = xQueueCreate(4, sizeof(_CANMSG));
}