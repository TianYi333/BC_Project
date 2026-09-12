/**
 * @file shell_port.c
 * @author Letter (NevermindZZT@gmail.com)
 * @brief
 * @version 0.1
 * @date 2019-02-22
 *
 * @copyright (c) 2019 Letter
 *
 */

#include "FreeRTOS.h"
#include "task.h"
#include "shell.h"
#include "usart.h"
#include "stm32h7xx_hal.h"
#include "usart.h"
#include "log.h"
#include "semphr.h"
#include "shell_port.h"

Shell shell;
char shellBuffer[512];

#define DEBUG_PORT            huart2
uint8_t __attribute__((section("._D3_DMA_SWRAM"))) debug_redata;

//使用尾行模式
void uartLogWrite(char *buffer, short len);
// level 显示的日志等级，高于level的等级不显示，具体的日志等级可以看log.h文件
Log uartLog =
		{ .write = uartLogWrite, .active = LOG_ENABLE, .level = LOG_DEBUG };

void uartLogWrite(char *buffer, short len) {
	if (uartLog.shell) {
		shellWriteEndLine(uartLog.shell, buffer, len);
	}
}

static __attribute__((section("._DTC_FSRAM")))  SemaphoreHandle_t shellMutex;
/**
 * @brief 用户shell上锁
 *
 * @param shell shell
 *
 * @return int 0
 */
int userShellLock(Shell *shell) {
	xSemaphoreTake(shellMutex, portMAX_DELAY);
	return 0;
}

/**
 * @brief 用户shell解锁
 *
 * @param shell shell
 *
 * @return int 0
 */
int userShellUnlock(Shell *shell) {
	xSemaphoreGive(shellMutex);
	return 0;
}

/**
 * @brief 用户shell读
 *
 * @param data 数据
 * @param len 数据长度
 *
 * @return short 实际读取到
 */
short userShellRead(char *data, unsigned short len) {
	if (HAL_UART_Receive(&DEBUG_PORT, (uint8_t*) data, len, 2000) != HAL_OK) {
		return 0;
	} else {
		return 1;
	}

}

/**
 * @brief 用户shell写
 *
 * @param data 数据
 * @param len 数据长度
 *
 * @return short 实际写入的数据长度
 */
short userShellWrite(char *data, unsigned short len) {
	HAL_UART_Transmit(&DEBUG_PORT, (uint8_t*) data, len, 0x1FF);
	return len;
}

/**
 * @brief 用户shell初始化
 *
 */
void userShellInit(void) {
	shellMutex = xSemaphoreCreateMutex();

	shell.write = userShellWrite;
	//shell.read = userShellRead;
	//shell.lock = userShellLock;
	//shell.unlock = userShellUnlock;
	shellInit(&shell, shellBuffer, 512);
	//添加log初始化
	logRegister(&uartLog, &shell);
	//xTaskCreate(shellTask, "shell", 256, &shell, 5, NULL);
	//HAL_UART_Receive_IT(&DEBUG_PORT, (uint8_t*) &debug_redata, 1);
	HAL_UARTEx_ReceiveToIdle_DMA(&DEBUG_PORT, &debug_redata, 10);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)//Size参数是本次接受到的数据长度
{
	if (huart->Instance == USART2) {

		shellHandler(&shell, debug_redata);
		HAL_UARTEx_ReceiveToIdle_DMA(&DEBUG_PORT, &debug_redata, 10);	//重新启动接收
	}
}
//void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
//	if (huart->Instance == USART2) //判断串口号
//	{
//
//		shellHandler(&shell, debug_redata);
//
//		HAL_UART_Receive_IT(&huart2, (uint8_t*) &debug_redata, 1);
//	}
//}
