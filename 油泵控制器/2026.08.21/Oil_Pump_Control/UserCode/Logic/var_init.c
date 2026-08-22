/*
  var_init.c
  @brief 变量初始化函数，主要用于初始化系统配置参数和注油点配置参数的初始值
  Created on: Oct 15, 2025
*/

#include "var_init.h"


/**
 * @fn void var_init()
 * @brief 变量初始化函数，调用其他初始化函数进行系统配置参数、注油点配置参数、系统状态、任务对象列表、电机位置列表的初始化
 *
 */
void var_init(void) {
    /* 初始化系统配置参数和注油点配置参数 */
  init_mb_reg();
  Cfg_SetDefault();   //填充出厂默认参数
  SysStatus_Init();   //初始化运行状态
}



/**
 * @fn void init_mb_reg()
 * @brief modbus寄存器复位函数
 *
 */
void init_mb_reg() {
  for (uint16_t i = 0; i < REG_Holding_NREGS; i++) {
    usRegHoldingBuf[i] = 0;
  }
}
