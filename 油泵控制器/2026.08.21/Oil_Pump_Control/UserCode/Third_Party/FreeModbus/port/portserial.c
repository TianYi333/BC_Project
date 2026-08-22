/*
 * FreeModbus Libary: BARE Port
 * Copyright (C) 2006 Christian Walter <wolti@sil.at>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * File: $Id$
 */

#include "port.h"

/* ----------------------- STM32 includes ----------------------------------*/
#include "stm32h7xx_hal.h"
#include "usart.h"
#include <string.h>

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb.h"
#include "mbport.h"

/* ----------------------- Defines ------------------------------------------*/
#define huartx           huart1
#define USARTx           USART1
#define MB_RTU_MIN_FRAME_LEN  4U    // RTU最小有效帧长度，过滤空帧/残帧

extern DMA_HandleTypeDef hdma_usart1_rx; // 新增 DMA 句柄
extern DMA_HandleTypeDef hdma_usart1_tx; // 新增发送 DMA 句柄

#define MB_SER_PDU_SIZE_MAX     512     /*!< Modbus RTU 帧的最大尺寸 */
volatile uint8_t ucLocalSlaveAddress = 1; //本机modbus从地址
extern volatile __attribute__((section("._D3_DMA_SWRAM")))        UCHAR ucRTUBuf[MB_SER_PDU_SIZE_MAX]; // 用于存储接收的 Modbus RTU 数据的缓冲区
extern volatile USHORT usRcvBufferPos; // 接收缓冲区中已存储数据的位置
extern volatile uint8_t slave_addr;

#define RS485_DIR_RECV()  HAL_GPIO_WritePin(RS485_2_RTS_GPIO_Port, RS485_2_RTS_Pin, GPIO_PIN_RESET)
#define RS485_DIR_SEND() HAL_GPIO_WritePin(RS485_2_RTS_GPIO_Port, RS485_2_RTS_Pin, GPIO_PIN_SET)
static BaseType_t serial_restart_req = pdFALSE;

// 防频繁重启节流（新增，防止干扰风暴反复复位串口）
static uint32_t last_restart_tick = 0U;

/* ----------------------- static functions ---------------------------------*/
static void prvvUARTTxReadyISR(void);
int xMBPortSerialRestart(void);
void SetSerialRestartRequest(void);
BaseType_t GetSerialRestartRequestAndClear(void);

/* ----------------------- Start implementation -----------------------------*/
static void VolatileBufClear(volatile uint8_t *buf, uint16_t len)
{
    for(uint16_t i = 0; i < len; i++)
    {
        buf[i] = 0U;
    }
}

void SetSerialRestartRequest(void)
{
    uint32_t now = xTaskGetTickCount();
    // 节流：500ms内禁止重复发起重启请求
    if ((now - last_restart_tick) < pdMS_TO_TICKS(500U))
    {
        return;
    }
    last_restart_tick = now;

    serial_restart_req = pdTRUE;
    // 重点：置位标志后，复用原生事件唤醒mbif任务
    xMBPortEventPost(EV_FRAME_RECEIVED);
}

BaseType_t GetSerialRestartRequestAndClear(void)
{
    BaseType_t ret = serial_restart_req;
    serial_restart_req = pdFALSE;
    return ret;
}

void vMBPortSerialEnable(BOOL xRxEnable, BOOL xTxEnable) {
	/* If xRXEnable enable serial receive interrupts. If xTxENable enable
	 * transmitter empty interrupts.
	 */
	if (xRxEnable) {

		__HAL_UART_DISABLE_IT(&huartx, UART_IT_TXE);    // 禁止发送空中断
		while (__HAL_UART_GET_FLAG(&huartx,UART_FLAG_TC) != SET)    //等待发送结束
		{
			/* code */
		}
		__HAL_UART_CLEAR_IT(&huartx, UART_IT_TXE);
		RS485_DIR_RECV();
		HAL_UART_DMAStop(&huartx);// 先停止DMA，确保状态复位，再重新启动
		if (HAL_UART_Receive_DMA(&huartx, (uint8_t*) ucRTUBuf, MB_SER_PDU_SIZE_MAX) != HAL_OK) 
		{
			// 处理错误
		}
		__HAL_UART_CLEAR_IDLEFLAG(&huartx);// 先清标志再开中断，避免启动瞬间误触发
		__HAL_UART_ENABLE_IT(&huartx, UART_IT_IDLE); // 使能空闲中断
	}

	if (xTxEnable) {
		__HAL_UART_DISABLE_IT(&huartx, UART_IT_IDLE); // 禁止空闲中断
		HAL_UART_DMAStop(&huartx); // 停止 DMA 接收
		RS485_DIR_SEND();
		__HAL_UART_CLEAR_FLAG(&huartx, UART_FLAG_TXE);
		__HAL_UART_ENABLE_IT(&huartx, UART_IT_TXE);    // 使能发送空中断
	}

}

BOOL xMBPortSerialInit(UCHAR ucPORT, ULONG ulBaudRate, UCHAR ucDataBits,
		eMBParity eParity) {
	HAL_UART_DeInit(&huartx);

	huartx.Instance = USARTx;
	huartx.Init.BaudRate = ulBaudRate;
	huartx.Init.StopBits = UART_STOPBITS_1;
	huartx.Init.Mode = UART_MODE_TX_RX;
	huartx.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huartx.Init.OverSampling = UART_OVERSAMPLING_16;
	huartx.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	huartx.Init.ClockPrescaler = UART_PRESCALER_DIV1;
	huartx.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

	switch (eParity) {
	case MB_PAR_ODD:
		huartx.Init.Parity = UART_PARITY_ODD;
		huartx.Init.WordLength = UART_WORDLENGTH_9B;
		break;
	case MB_PAR_EVEN:
		huartx.Init.Parity = UART_PARITY_EVEN;
		huartx.Init.WordLength = UART_WORDLENGTH_9B;
		break;
	case MB_PAR_NONE:
		huartx.Init.Parity = UART_PARITY_NONE;
		huartx.Init.WordLength = UART_WORDLENGTH_8B;
		break;
	}

	return HAL_UART_Init(&huartx) == HAL_OK ? TRUE : FALSE;

}

BOOL xMBPortSerialPutByte(CHAR ucByte) {
	/* Put a byte in the UARTs transmit buffer. This function is called
	 * by the protocol stack if pxMBFrameCBTransmitterEmpty( ) has been
	 * called. */

	USART1->TDR = ucByte;
	return TRUE;

}

/* Create an interrupt handler for the transmit buffer empty interrupt
 * (or an equivalent) for your target processor. This function should then
 * call pxMBFrameCBTransmitterEmpty( ) which tells the protocol stack that
 * a new character can be sent. The protocol stack will then call
 * xMBPortSerialPutByte( ) to send the character.
 */
void prvvUARTTxReadyISR(void) {
	pxMBFrameCBTransmitterEmpty();
}

void USART1_IRQHandler(void) {
	uint8_t dummy;
	if (__HAL_UART_GET_FLAG(&huartx, UART_FLAG_IDLE)) // 空闲中断标记被置位
	{
		uint32_t tmpreg;
		tmpreg = huartx.Instance->ISR;          // 读取状态寄存器
		tmpreg = huartx.Instance->RDR;          // 读取接收寄存器
		huartx.Instance->ICR = UART_CLEAR_IDLEF; // 【关键】清零IDLE标志
		(void)tmpreg;
		usRcvBufferPos = MB_SER_PDU_SIZE_MAX - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx); // 计算接收到的数据长度
		//最小帧长校验，空帧/残帧直接丢弃，不触发任何逻辑
        if(usRcvBufferPos < MB_RTU_MIN_FRAME_LEN)
        {
            HAL_UART_DMAStop(&huartx);
            usRcvBufferPos = 0U;
			VolatileBufClear(ucRTUBuf, MB_SER_PDU_SIZE_MAX);
            HAL_UART_Receive_DMA(&huartx, (uint8_t *)ucRTUBuf, MB_SER_PDU_SIZE_MAX);
        }
		else
		{
			// 将接收到的从机地址存储到指定位置
			if (ucRTUBuf[0] == slave_addr|| ucRTUBuf[0] == MB_ADDRESS_BROADCAST) 
			{
				xMBPortEventPost(EV_FRAME_RECEIVED); // 通知协议栈有新帧接收
			} 
			else 
			{
				HAL_UART_DMAStop(&huartx);
				usRcvBufferPos = 0U;
				VolatileBufClear(ucRTUBuf, MB_SER_PDU_SIZE_MAX);
				HAL_UART_Receive_DMA(&huartx, (uint8_t *)ucRTUBuf, MB_SER_PDU_SIZE_MAX);

			}
		}
	} 
	else if (__HAL_UART_GET_FLAG(&huartx, UART_FLAG_TXE)) // 发送为空中断标记被置位		
	{
		__HAL_UART_CLEAR_FLAG(&huartx, UART_FLAG_TXE); // 清除中断标记
		// 只调用一次发送函数，避免重复写TDR导致数据覆盖
		prvvUARTTxReadyISR();

	} 
	else if (__HAL_UART_GET_FLAG(&huartx, UART_FLAG_RXNE)) // 接收缓冲区非空中断标记被置位	
	{
		dummy = (uint8_t)(huartx.Instance->RDR);
		__HAL_UART_CLEAR_FLAG(&huartx, UART_FLAG_RXNE); // 清除中断标记
		// 若有需要可添加接收数据处理逻辑
	} 
	else if (__HAL_UART_GET_FLAG(&huartx, UART_FLAG_ORE) || // 溢出错误
			__HAL_UART_GET_FLAG(&huartx, UART_FLAG_NE) || // 噪声错误
			__HAL_UART_GET_FLAG(&huartx, UART_FLAG_FE) || // 帧错误
			__HAL_UART_GET_FLAG(&huartx, UART_FLAG_PE))    // 奇偶校验错误
	{
		// 读RDR排空数据，防止ORE错误反复触发中断
		dummy = (uint8_t)(huartx.Instance->RDR);
		__HAL_UART_CLEAR_FLAG(&huartx, UART_FLAG_ORE | UART_FLAG_NE | UART_FLAG_FE | UART_FLAG_PE |UART_FLAG_RXNE); // 清除错误中断标记
		// 若有需要可添加错误处理逻辑
        SetSerialRestartRequest();
	}
	(void)dummy;
}

int xMBPortSerialRestart(void)
{
    taskENTER_CRITICAL();
    // 停止DMA接收
	//HAL_UART_DMAStop(&huartx);
    usRcvBufferPos = 0U;
	VolatileBufClear(ucRTUBuf, MB_SER_PDU_SIZE_MAX);

    // 完整重新初始化串口（原MX_USART1_UART_Init逻辑，任务中调用安全）
    HAL_UART_DeInit(&huartx);
    MX_USART1_UART_Init();

    // 重启DMA接收与空闲中断
	if (HAL_UART_Receive_DMA(&huartx, (uint8_t *)ucRTUBuf, MB_SER_PDU_SIZE_MAX) == HAL_OK)
	{
		__HAL_UART_CLEAR_IDLEFLAG(&huartx);
		__HAL_UART_ENABLE_IT(&huartx, UART_IT_IDLE);
	}
	vMBPortSerialEnable(TRUE, FALSE);
    taskEXIT_CRITICAL();
	return 1;
}

