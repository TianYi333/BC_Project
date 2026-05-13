/*
 * mbReg.h
 *
 *  Created on: Oct 16, 2025
 *      Author: 28038
 */

#ifndef THIRD_PARTY_FREEMODBUS_INCLUDE_MBREG_H_
#define THIRD_PARTY_FREEMODBUS_INCLUDE_MBREG_H_

/* ----------------------- Defines ------------------------------------------*/
#define REG_INPUT_START        0
#define REG_INPUT_NREGS        100
#define REG_Coils_START        0
#define REG_Coils_NREGS        100
#define REG_Holding_START      0
#define REG_Holding_NREGS      500
#define REG_Discrete_START     0
#define REG_Discrete_NREGS     100

extern uint16_t __attribute__((section("._DTC_FSRAM"))) usRegInputBuf[REG_INPUT_NREGS];
extern uint8_t __attribute__((section("._DTC_FSRAM"))) usRegCoilsBuf[REG_Coils_NREGS];
extern uint16_t __attribute__((section("._DTC_FSRAM"))) usRegHoldingBuf[REG_Holding_NREGS];
extern uint8_t __attribute__((section("._DTC_FSRAM"))) usRegDiscreteBuf[REG_Discrete_NREGS];

#endif /* THIRD_PARTY_FREEMODBUS_INCLUDE_MBREG_H_ */
