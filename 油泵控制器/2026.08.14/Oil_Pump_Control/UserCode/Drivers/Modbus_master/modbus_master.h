#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include "stm32h7xx_hal.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <stdint.h>
#include <string.h>
#include "main_logic.h"

#define LOG_mb(fmt, ...) (void)0
//#define LOG_mb(fmt, ...) printf("[mb] " fmt "\r\n", ##__VA_ARGS__)

#define MOTOR_SLAVE_ADDR        1U          // 电机Modbus地址，自行修改和电机匹配
#define LIQUID_SLAVE_ADDR       127        // 液位传感器Modbus地址，自行修改和传感器匹配
#define LIQUID_REG_LEVEL        0x0A0F      // 液位寄存器起始地址，以传感器手册为准

#define MOTOR_COMM_ERR_MAX      3U      // 连续3次通讯失败判定链路丢失

// 485 DIR引脚 PG3
#define RS485_DIR_PIN      GPIO_PIN_3
#define RS485_DIR_PORT     GPIOG

// Modbus标准功能码
#define MB_FUNC_READ_HOLDING   0x03
#define MB_FUNC_WRITE_SINGLE   0x06
#define MB_FUNC_WRITE_MULTI    0x10

#define MB_TX_BUF_LEN 64
#define MB_RX_BUF_LEN 64
#define MB_RX_TIMEOUT pdMS_TO_TICKS(20)
#define MB_MSG_QUEUE_DEPTH 8
#define MB_MSG_BUF_SIZE     8U          /* 消息内嵌寄存器缓冲区 */

//====Modbus操作枚举、消息结构体====
typedef enum
{
    MB_OP_READ_HOLD,
    MB_OP_WRITE_SINGLE,
    MB_OP_WRITE_MULTI,
} MB_OP_E;

//单条总线请求消息【修改：增加内嵌data_buf，解决p_data栈野指针】
typedef struct
{
    MB_OP_E op;// 操作类型，读保持寄存器/单写寄存器/多写寄存器
    uint8_t slave_addr;// 从机地址
    uint16_t reg_start;// 寄存器起始地址，单写时为0x0000
    uint16_t reg_num;// 读保持寄存器/多寄存器写专用数量
    uint16_t val_single;        // 单写专用值
    uint16_t data_buf[MB_MSG_BUF_SIZE]; // 内嵌数据缓冲区
    uint16_t *p_data;           // 读写数据缓冲区，指向本结构体data_buf
    osSemaphoreId_t ack_sem;    // 回执信号量，通知调用方完成
    int ret_code;               // 操作返回值 0成功 负数失败
} MB_MSG_T;

// 全局收发缓冲区
extern uint8_t mb_tx_buf[MB_TX_BUF_LEN];
extern uint8_t mb_rx_buf[MB_RX_BUF_LEN];
// 主机串口句柄 USART3
extern UART_HandleTypeDef huart3;
// Modbus互斥【替换原二元信号量mb_semaphore】
extern osMutexId_t mb_bus_mutex;
// 接收完成信号量（DMA+空闲中断唤醒）
extern osSemaphoreId_t mb_rx_semaphore;
// FreeRTOS原生消息队列
extern QueueHandle_t mb_msg_queue;

extern uint16_t motor_comm_err_cnt;// 电机Modbus连续通讯错误计数
extern uint16_t liquid_comm_err_cnt;// 液位传感器Modbus连续通讯错误

void HAL_Delay_us(uint32_t us);
void MB_Semaphore_Init(void);
void ModbusMasterTask(void *argument);
uint16_t mb_crc16(uint8_t *data, uint16_t len);
void rs485_tx_en(void);
void rs485_rx_en(void);
//底层仅总线任务内部调用
int mb_master_read_holding(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_out);
int mb_master_write_single(uint8_t slave_addr, uint16_t reg_addr, uint16_t val);
int mb_master_write_multi(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_in);

//轮询专用电机读接口（仅ModbusMasterTask内部）
uint16_t Motor_ReadErrCode(void);
uint16_t Motor_ReadStatusWord(void);
uint16_t Motor_ReadRunMode(void);
float Motor_ReadRealSpeed(void);
uint16_t Motor_ReadAcc(void);
uint16_t Motor_ReadDec(void);
uint16_t LiquidSensor_ReadLevel(void);

//对外统一提交接口（其他任务调用）
int MB_SubmitRequest(MB_MSG_T *req_msg, uint32_t wait_ms);

/* 电机统一下发对外API，业务层全部调用这组，禁止直接Motor_SetXXX */
int MotorCtrl_SubmitSetMode(uint16_t mode_val);
int MotorCtrl_SubmitSetSpeed(float speed_rpm);
int MotorCtrl_SubmitSetAcc(uint16_t acc_val);
int MotorCtrl_SubmitSetDec(uint16_t dec_val);

// 串口回调
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size);

#endif


