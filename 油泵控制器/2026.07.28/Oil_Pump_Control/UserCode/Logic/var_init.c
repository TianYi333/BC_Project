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
  init_main_config();
  init_mb_reg();
}

/**
 * @fn void init_main_config()
 * @brief 配置初始化函数,设置系统配置参数和注油点配置参数的初始值
 *
 */
void init_main_config(void) {
  // 初始化系统配置参数
  main_config.slaveID = 1;
  main_config.BaudRate = 115200;
  injector_config.maxMotorSpeed = 1000;
  injector_config.baseMotorSpeed = 500;
  main_config.commMode = 0;
  main_config.workMode = 0;
  strncpy(main_config.deviceSN, "1234567890SN",
          sizeof(main_config.deviceSN) - 1);
  strncpy(main_config.ipAddress, "192.168.1.100",
          sizeof(main_config.ipAddress) - 1);
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
