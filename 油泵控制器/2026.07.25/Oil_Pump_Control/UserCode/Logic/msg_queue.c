#include "msg_queue.h"
#include "net_comm_task.h"

QueueHandle_t QUEUE_BUZZ = NULL;//蜂鸣器操作消息队列
QueueHandle_t QUEUE_OLED = NULL;//oled错误消息队列
QueueHandle_t QUEUE_CONN = NULL;//配置修改消息队列
QueueHandle_t QUEUE_KEY = NULL;//键盘操作消息队列
QueueHandle_t QUEUE_MET_SEN = NULL;//金属接近传感器队列
QueueHandle_t msgQueue_ID_CAN1 = NULL;//CAN1其它消息队列
QueueHandle_t msgQueue_ID_CAN2 = NULL;//CAN2其它消息队列
QueueHandle_t udp_msg_queue = NULL;//UDP消息队列
QueueHandle_t xTcpTaskQueue = NULL;//TCP任务队列
QueueHandle_t xTimerReqQueue = NULL;//定时上报任务队列
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
	udp_msg_queue = xQueueCreate(UDP_MSG_QUEUE_LEN, sizeof(UdpMsgTypeDef));
  	xTcpTaskQueue = xQueueCreate(TCP_TASK_QUEUE_LEN, sizeof(TaskInfo_t));
  	xTimerReqQueue = xQueueCreate(8, sizeof(TaskInfo_t));
}

