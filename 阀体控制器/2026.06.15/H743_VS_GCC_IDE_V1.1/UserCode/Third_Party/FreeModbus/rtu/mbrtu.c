/*
 * FreeModbus Libary: A portable Modbus implementation for Modbus ASCII/RTU.
 * Copyright (c) 2006-2018 Christian Walter <cwalter@embedded-solutions.at>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/* ----------------------- System includes ----------------------------------*/
#include "stdlib.h"
#include "string.h"

/* ----------------------- Platform includes --------------------------------*/
#include "port.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb.h"
#include "mbrtu.h"
#include "mbframe.h"

#include "mbcrc.h"
#include "mbport.h"

/* ----------------------- Defines ------------------------------------------*/
#define MB_SER_PDU_SIZE_MIN     4       /*!< Modbus RTU 帧的最小尺寸 */
#define MB_SER_PDU_SIZE_MAX     256     /*!< Modbus RTU 帧的最大尺寸 */
#define MB_SER_PDU_SIZE_CRC     2       /*!< PDU 中 CRC 字段的大小 */
#define MB_SER_PDU_ADDR_OFF     0       /*!< 从机地址在串行 PDU 中的偏移量 */
#define MB_SER_PDU_PDU_OFF      1       /*!< Modbus-PDU 在串行 PDU 中的偏移量 */

/* ----------------------- Type definitions ---------------------------------*/
typedef enum {
	STATE_RX_INIT, /*!< 接收器处于初始状态 */
	STATE_RX_IDLE, /*!< 接收器处于空闲状态 */
	STATE_RX_RCV, /*!< 正在接收帧 */
	STATE_RX_ERROR /*!< 如果帧无效 */
} eMBRcvState;

typedef enum {
	STATE_TX_IDLE, /*!< 发送器处于空闲状态 */
	STATE_TX_XMIT /*!< 发送器处于传输状态 */
} eMBSndState;

/* ----------------------- Static variables ---------------------------------*/
static volatile eMBSndState eSndState; // 发送器状态，volatile 确保在多线程或中断环境下能正确访问
static volatile eMBRcvState eRcvState; // 接收器状态，volatile 确保在多线程或中断环境下能正确访问

volatile __attribute__((section("._D3_DMA_SWRAM")))    UCHAR ucRTUBuf[MB_SER_PDU_SIZE_MAX]; // 用于存储接收的 Modbus RTU 数据的缓冲区

static volatile UCHAR *pucSndBufferCur; // 指向发送缓冲区当前位置的指针
static volatile USHORT usSndBufferCount; // 发送缓冲区中剩余要发送的字节数

volatile USHORT usRcvBufferPos; // 接收缓冲区中已存储数据的位置

/* ----------------------- Start implementation -----------------------------*/
eMBErrorCode eMBRTUInit(UCHAR ucSlaveAddress, UCHAR ucPort, ULONG ulBaudRate,
		eMBParity eParity) {
	eMBErrorCode eStatus = MB_ENOERR;

	(void) ucSlaveAddress;
	ENTER_CRITICAL_SECTION();

	/* Modbus RTU uses 8 Databits. */
	if (xMBPortSerialInit(ucPort, ulBaudRate, 8, eParity) != TRUE) {
		eStatus = MB_EPORTERR;
	} else {

	}
	EXIT_CRITICAL_SECTION();

	return eStatus;
}

void eMBRTUStart(void) {
	ENTER_CRITICAL_SECTION(); // 进入临界区
	eRcvState = STATE_RX_IDLE; // 设置接收器状态为初始状态
	vMBPortSerialEnable( TRUE, FALSE); // 使能串口接收，禁用串口发送
	EXIT_CRITICAL_SECTION(); // 退出临界区
}

void eMBRTUStop(void) {
	ENTER_CRITICAL_SECTION(); // 进入临界区
	vMBPortSerialEnable( FALSE, FALSE); // 禁用串口接收和发送
	EXIT_CRITICAL_SECTION(); // 退出临界区
}

eMBErrorCode eMBRTUReceive(UCHAR *pucRcvAddress, UCHAR **pucFrame,
		USHORT *pusLength) {
	BOOL xFrameReceived = FALSE; // 标记是否接收到有效帧
	eMBErrorCode eStatus = MB_ENOERR; // 错误状态码，初始设为无错误

	ENTER_CRITICAL_SECTION(); // 进入临界区
	assert(usRcvBufferPos < MB_SER_PDU_SIZE_MAX); // 断言确保接收缓冲区位置在有效范围内

	/* 长度和 CRC 检查 */
	if ((usRcvBufferPos >= MB_SER_PDU_SIZE_MIN)
			&& (usMBCRC16((UCHAR*) ucRTUBuf, usRcvBufferPos) == 0)) {
		/* 保存地址字段。所有帧都传递到上层，
		 * 是否使用该帧的决策在那里进行。
		 */
		*pucRcvAddress = ucRTUBuf[MB_SER_PDU_ADDR_OFF]; // 将接收到的从机地址存储到指定位置

		/* Modbus-PDU 的总长度是 Modbus 串行线路 PDU 减去
		 * 地址字段大小和 CRC 校验和。
		 */
		*pusLength = (USHORT) (usRcvBufferPos - MB_SER_PDU_PDU_OFF
				- MB_SER_PDU_SIZE_CRC); // 计算有效数据长度

		/* 将 Modbus PDU 的起始位置返回给调用者。 */
		*pucFrame = (UCHAR*) &ucRTUBuf[MB_SER_PDU_PDU_OFF]; // 设置指向有效数据的指针
		xFrameReceived = TRUE; // 标记接收到有效帧
	} else {
		eStatus = MB_EIO; // 如果帧无效，设置错误状态为输入输出错误
	}

	EXIT_CRITICAL_SECTION(); // 退出临界区
	return eStatus; // 返回错误状态码
}

eMBErrorCode eMBRTUSend(UCHAR ucSlaveAddress, const UCHAR *pucFrame,
		USHORT usLength) {
	eMBErrorCode eStatus = MB_ENOERR; // 错误状态码，初始设为无错误
	USHORT usCRC16; // 用于存储 CRC16 校验和

	ENTER_CRITICAL_SECTION(); // 进入临界区

	/* 检查接收器是否仍处于空闲状态。如果不是，说明我们处理接收到的帧太慢，
	 * 并且主机在网络上发送了另一个帧。我们必须中止发送该帧。
	 */
	if (eRcvState == STATE_RX_IDLE) {
		/* Modbus-PDU 之前的第一个字节是从机地址。 */
		pucSndBufferCur = (UCHAR*) pucFrame - 1; // 设置发送缓冲区指针指向从机地址位置
		usSndBufferCount = 1; // 初始化发送缓冲区计数为 1（从机地址占 1 字节）

		/* 现在将 Modbus-PDU 复制到 Modbus 串行线路 PDU 中。 */
		pucSndBufferCur[MB_SER_PDU_ADDR_OFF] = ucSlaveAddress; // 将从机地址存储到发送缓冲区
		usSndBufferCount += usLength; // 更新发送缓冲区计数为从机地址和有效数据长度之和

		/* 计算 Modbus 串行线路 PDU 的 CRC16 校验和。 */
		usCRC16 = usMBCRC16((UCHAR*) pucSndBufferCur, usSndBufferCount); // 计算 CRC16 校验和
		ucRTUBuf[usSndBufferCount++] = (UCHAR) (usCRC16 & 0xFF); // 将 CRC16 低字节存储到缓冲区
		ucRTUBuf[usSndBufferCount++] = (UCHAR) (usCRC16 >> 8); // 将 CRC16 高字节存储到缓冲区

		/* 激活发送器。 */
		eSndState = STATE_TX_XMIT; // 设置发送器状态为传输状态
		vMBPortSerialEnable( FALSE, TRUE); // 禁用串口接收，使能串口发送
	} else {
		eStatus = MB_EIO; // 如果接收器不处于空闲状态，设置错误状态为输入输出错误
	}
	EXIT_CRITICAL_SECTION(); // 退出临界区
	return eStatus; // 返回错误状态码
}

BOOL xMBRTUReceiveFSM(void) {
	BOOL xTaskNeedSwitch = FALSE; // 标记是否需要任务切换
	assert(eSndState == STATE_TX_IDLE); // 断言确保发送器处于空闲状态

	switch (eRcvState) {
	case STATE_RX_INIT:
		eRcvState = STATE_RX_IDLE; // 直接切换到空闲状态
		break;

		/* 在错误状态下，我们等待直到损坏的帧中的所有字符都被传输。 */
	case STATE_RX_ERROR:
		eRcvState = STATE_RX_IDLE; // 直接切换到空闲状态
		break;

	case STATE_RX_IDLE:
		// 无需处理，等待空闲中断触发接收完成事件
		break;

	case STATE_RX_RCV:
		// 无需处理，等待空闲中断触发接收完成事件
		break;
	}
	return xTaskNeedSwitch; // 返回是否需要任务切换的标记
}

BOOL xMBRTUTransmitFSM(void) {
	BOOL xNeedPoll = FALSE; // 标记是否需要轮询

	assert(eRcvState == STATE_RX_IDLE); // 断言确保接收器处于空闲状态

	switch (eSndState) {
	/* 如果发送器处于空闲状态，我们不应该收到发送器事件。  */
	case STATE_TX_IDLE:
		/* 启用接收器/禁用发送器。 */
		vMBPortSerialEnable( TRUE, FALSE); // 使能串口接收，禁用串口发送
		break;

	case STATE_TX_XMIT:
		/* 检查我们是否完成发送。 */
		if (usSndBufferCount != 0) {
			xMBPortSerialPutByte((CHAR) *pucSndBufferCur); // 发送缓冲区中的当前字节
			pucSndBufferCur++; /* 发送缓冲区中的下一个字节。 */
			usSndBufferCount--; // 减少待发送字节数
		} else {
			xNeedPoll = xMBPortEventPost(EV_FRAME_SENT); // 发送帧发送完成事件
			/* 禁用发送器。这可以防止另一个发送缓冲区为空中断。 */
			vMBPortSerialEnable( TRUE, FALSE); // 使能串口接收，禁用串口发送
			eSndState = STATE_TX_IDLE; // 设置发送器状态为空闲状态
		}
		break;
	}

	return xNeedPoll; // 返回是否需要轮询的标记
}

