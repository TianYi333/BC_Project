/*
 * FreeModbus Libary: STR71x Demo Application
 * Copyright (C) 2006 Christian Walter <wolti@sil.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * File: $Id$
 */

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb.h"
#include "mbutils.h"
#include "mbReg.h"
#include "msg_queue.h"
#include "syncif.h"

/* ----------------------- Static variables ---------------------------------*/
uint16_t usRegInputStart = REG_INPUT_START;
uint16_t usRegInputBuf[REG_INPUT_NREGS];
uint16_t usRegCoilsStart = REG_Coils_START;
uint8_t usRegCoilsBuf[REG_Coils_NREGS];
uint16_t usRegHoldingStart = REG_Holding_START;
uint16_t usRegHoldingBuf[REG_Holding_NREGS];
uint16_t usRegDiscreteStart = REG_Discrete_START;
uint8_t usRegDiscreteBuf[REG_Discrete_NREGS];

/* ----------------------- Start implementation -----------------------------*/
eMBErrorCode eMBRegInputCB(UCHAR *pucRegBuffer, USHORT usAddress,
		USHORT usNRegs) {
	eMBErrorCode eStatus = MB_ENOERR;
	SHORT iNRegs = (SHORT) usNRegs;
	int iRegIndex;

	if ((usAddress >= REG_INPUT_START)
			&& (usAddress + usNRegs <= REG_INPUT_START + REG_INPUT_NREGS)) {
		iRegIndex = (int) (usAddress - usRegInputStart - 1);

		while (iNRegs > 0) {
			*pucRegBuffer++ = (unsigned char) (usRegInputBuf[iRegIndex] >> 8);
			*pucRegBuffer++ = (unsigned char) (usRegInputBuf[iRegIndex] & 0xFF);
			iRegIndex++;
			iNRegs--;
		}
	} else {
		eStatus = MB_ENOREG;
	}

	return eStatus;
}

eMBErrorCode eMBRegCoilsCB(UCHAR *pucRegBuffer, USHORT usAddress,
		USHORT usNCoils, eMBRegisterMode eMode) {
	eMBErrorCode eStatus = MB_ENOERR;
	SHORT iNCoils = (SHORT) usNCoils;
	int iRegIndex;

	if ((usAddress >= REG_Coils_START)
			&& (usAddress + usNCoils <= REG_Coils_START + REG_Coils_NREGS)) {
		iRegIndex = (int) (usAddress - usRegCoilsStart - 1);

		switch (eMode) {
		case MB_REG_READ:
			taskENTER_CRITICAL();
			while (iNCoils > 0) {
				*pucRegBuffer++ = xMBUtilGetBits(usRegCoilsBuf, iRegIndex,
						(unsigned char) (iNCoils > 8 ? 8 : iNCoils));
				iRegIndex += 8;
				iNCoils -= 8;
			}
			taskEXIT_CRITICAL();
			break;
		case MB_REG_WRITE:
			taskENTER_CRITICAL();
			while (iNCoils > 0) {
				xMBUtilSetBits(usRegCoilsBuf, iRegIndex,
						(unsigned char) (iNCoils > 8 ? 8 : iNCoils),
						*pucRegBuffer++);
				iRegIndex += 8;
				iNCoils -= 8;
			}
			taskEXIT_CRITICAL();
			break;
		}
	} else {
		eStatus = MB_ENOREG;
	}

	return eStatus;
}

eMBErrorCode eMBRegHoldingCB(UCHAR *pucRegBuffer, USHORT usAddress,
		USHORT usNRegs, eMBRegisterMode eMode) {
	eMBErrorCode eStatus = MB_ENOERR;
	SHORT iNRegs = (SHORT) usNRegs;
	_MB_REG mb_Msg = { .startAddr = usAddress - 1, .len = usNRegs };
	int iRegIndex;

	if ((usAddress >= REG_Holding_START)
			&& (usAddress + usNRegs <= REG_Holding_START + REG_Holding_NREGS)) {
		iRegIndex = (int) (usAddress - usRegHoldingStart - 1);
		switch (eMode) {
		case MB_REG_READ:
			taskENTER_CRITICAL();
			while (iNRegs > 0) {
				*pucRegBuffer++ = (unsigned char) (usRegHoldingBuf[iRegIndex]
						>> 8);
				*pucRegBuffer++ = (unsigned char) (usRegHoldingBuf[iRegIndex]
						& 0xFF);
				iRegIndex++;
				iNRegs--;
			}
			taskEXIT_CRITICAL();
			break;
		case MB_REG_WRITE:
			taskENTER_CRITICAL();
			while (iNRegs > 0) {
				usRegHoldingBuf[iRegIndex] = (uint16_t) *pucRegBuffer++ << 8;
				usRegHoldingBuf[iRegIndex] = (usRegHoldingBuf[iRegIndex]
						& 0xFF00) | *pucRegBuffer++;
				iRegIndex++;
				iNRegs--;
			}
			taskEXIT_CRITICAL();
		    xQueueOverwrite(QUEUE_CONN, &mb_Msg);
			break;
		}
	} else {
		eStatus = MB_ENOREG;
	}

	return eStatus;
}

eMBErrorCode eMBRegDiscreteCB(UCHAR *pucRegBuffer, USHORT usAddress,
		USHORT usNDiscrete) {
	eMBErrorCode eStatus = MB_ENOERR;
	SHORT iNDiscrete = (SHORT) usNDiscrete;
	int iRegIndex;

	if ((usAddress >= REG_Discrete_START)
			&& (usAddress + usNDiscrete
					<= REG_Discrete_START + REG_Discrete_NREGS)) {
		iRegIndex = (unsigned short) (usAddress - usRegDiscreteStart - 1);

		while (iNDiscrete > 0) {
			*pucRegBuffer++ = xMBUtilGetBits(usRegDiscreteBuf, iRegIndex,
					(unsigned char) (iNDiscrete > 8 ? 8 : iNDiscrete));
			iRegIndex += 8;
			iNDiscrete -= 8;
		}
	} else {
		eStatus = MB_ENOREG;
	}
	return eStatus;
}

