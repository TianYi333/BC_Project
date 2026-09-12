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
QueueHandle_t tcp_send_queue = NULL;//TCP发送消息消息队列
QueueHandle_t xRebootCmdQueue = NULL;//重启指令消息队列
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
	tcp_send_queue = xQueueCreate(32, sizeof(tcp_send_msg_t));
	xRebootCmdQueue = xQueueCreate(1, sizeof(RebootCmd_t));
}

