/**
 * @file msg_queue.h
 * @brief 消息队列头文件
 * @author 梁伟
 * @date 2024-06-15
 */
#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/* 按键消息队列句柄 */
extern __attribute__((section("._DTC_FSRAM"))) QueueHandle_t QUEUE_KEY;
/**
 * 蜂鸣器操作消息
 */
extern __attribute__((section("._DTC_FSRAM")))  QueueHandle_t QUEUE_BUZZ;
/**
 * oled错误消息
 */
extern __attribute__((section("._DTC_FSRAM")))   QueueHandle_t QUEUE_OLED;
/**
 * 配置修改消息
 */
extern __attribute__((section("._DTC_FSRAM")))   QueueHandle_t QUEUE_CONN;
/**
 * 键盘操作消息
 */
extern __attribute__((section("._DTC_FSRAM")))   QueueHandle_t QUEUE_KEY;
/**
 * 金属接近传感器
 */
extern __attribute__((section("._DTC_FSRAM")))   QueueHandle_t QUEUE_MET_SEN;
/**
 * CAN1其它消息
 */
extern __attribute__((section("._DTC_FSRAM")))   QueueHandle_t msgQueue_ID_CAN1;
/**
 * CAN2其它消息
 */
extern __attribute__((section("._DTC_FSRAM")))   QueueHandle_t msgQueue_ID_CAN2;


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
/*
 * modbus寄存器结构
 */
typedef struct mbReg {
	uint16_t startAddr;
	uint16_t len;
} _MB_REG;

/*
 * oled消息类型
 */
typedef enum {
    OLED_MSG_ERROR = 0,   // 错误消息
    OLED_MSG_UPDATE = 1,   // 更新显示消息
	OLED_MSG_NULL = 2   // 空消息
} OLED_MsgType;

/**
 * oled消息结构体
 */
typedef struct oledMsg {
    OLED_MsgType type; // 消息类型
    char data[32];     // 消息数据（例如错误信息）
} OLED_Msg;


/**
 * @brief 初始化消息队列
 */
void MsgQueue_Init(void);

#endif /* MSG_QUEUE_H */