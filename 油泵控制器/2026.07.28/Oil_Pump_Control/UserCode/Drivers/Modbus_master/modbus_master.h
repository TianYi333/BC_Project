#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include "stm32h7xx_hal.h"
#include "cmsis_os.h"
#include <stdint.h>
#include <string.h>
#include "main_logic.h"

#define LOG_mb(fmt, ...) (void)0
//#define LOG_mb(fmt, ...) printf("[R] " fmt "\r\n", ##__VA_ARGS__)

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

// 全局收发缓冲区
extern uint8_t mb_tx_buf[MB_TX_BUF_LEN];
extern uint8_t mb_rx_buf[MB_RX_BUF_LEN];
// 主机串口句柄 USART3
extern UART_HandleTypeDef huart3;
// Modbus互斥信号量
extern osSemaphoreId_t mb_semaphore;
extern uint16_t motor_comm_err_cnt;// 电机Modbus连续通讯错误计数
extern uint16_t liquid_comm_err_cnt;// 液位传感器Modbus连续通讯错误计数


void HAL_Delay_us(uint32_t us);
void MB_Semaphore_Init(void);
void ModbusMasterTask(void *argument);
uint16_t mb_crc16(uint8_t *data, uint16_t len);
void rs485_tx_en(void);
void rs485_rx_en(void);
int mb_master_read_holding(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_out);
int mb_master_write_single(uint8_t slave_addr, uint16_t reg_addr, uint16_t val);
int mb_master_write_multi(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_in);
uint16_t Motor_ReadErrCode(void);
uint16_t Motor_ReadStatusWord(void);
uint16_t Motor_ReadRunMode(void);
int Motor_SetRunMode(uint16_t mode_val);
int Motor_SetTargetSpeed(float speed_rpm);
float Motor_ReadRealSpeed(void);
int Motor_SetAcc(uint16_t acc_val);
uint16_t Motor_ReadAcc(void);
int Motor_SetDec(uint16_t dec_val);
uint16_t Motor_ReadDec(void);
uint16_t LiquidSensor_ReadLevel(void); 

#endif

