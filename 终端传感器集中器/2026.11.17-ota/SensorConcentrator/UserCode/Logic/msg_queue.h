#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

extern QueueHandle_t QUEUE_BUZZ;//蜂鸣器操作消息队列
extern QueueHandle_t QUEUE_OLED;//oled错误消息队列
extern QueueHandle_t QUEUE_CONN;//配置修改消息队列
extern QueueHandle_t QUEUE_KEY;//键盘操作消息队列
extern QueueHandle_t QUEUE_MET_SEN;//金属接近传感器队列
extern QueueHandle_t msgQueue_ID_CAN1;//CAN1其它消息队列
extern QueueHandle_t msgQueue_ID_CAN2;//CAN2其它消息队列
extern QueueHandle_t udp_msg_queue;//UDP消息队列
extern QueueHandle_t tcp_send_queue;//TCP发送消息消息队列
extern QueueHandle_t xRebootCmdQueue;//重启指令消息队列

#define mb_msg    1

/* 帧类型 */
typedef enum frame_type {
	DATA_FRAME = 0, /* 数据帧 */
	REMOTE_FRAME = 1, /* 远程帧 */

} _FRAME_TYPE;

/* 接收CAN报文结构体 */
typedef struct canmsg {
	uint16_t id; /* CANID */
	_FRAME_TYPE rtr; /* 远程帧，数据帧 */
	uint8_t len; /* CAN报文长度 */
	uint8_t buffer[8]; /* CAN报文内容 */

} _CANMSG;

/* modbus寄存器结构 */
typedef struct mbReg {
	uint16_t startAddr;
	uint16_t len;
} _MB_REG;

/* oled消息类型 */
typedef enum {
    OLED_MSG_ERROR = 0,   // 错误消息
    OLED_MSG_UPDATE = 1,   // 更新显示消息
	OLED_MSG_NULL = 2   // 空消息
} OLED_MsgType;

/* oled消息结构体 */
typedef struct oledMsg {
    OLED_MsgType type; // 消息类型
    char data[32];     // 消息数据（例如错误信息）
} OLED_Msg;


void Mutex_Init(void);
/* 初始化消息队列 */
void MsgQueue_Init(void);

#endif /* MSG_QUEUE_H */