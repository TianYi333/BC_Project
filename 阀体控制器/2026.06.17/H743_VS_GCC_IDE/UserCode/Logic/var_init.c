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
    /* 初始化注油点配置参数 */
  init_injector_config();
    /* 初始化系统状态 */
  init_main_sys_state();
    /* 初始化任务对象列表 */
  init_task_object_list();
    /* 初始化Q电机位置列表 */
  init_Q_motor_loc_list();
    /* 初始化P电机位置列表 */
  init_P_motor_loc_list();
    /* 初始化modbus寄存器 */
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
 * @fn void init_injector_config()
 * @brief 注油点配置初始化函数,设置注油点配置参数的初始值
 *
 */
void init_injector_config(void) {
  // 初始化注油点配置参数
  for (uint8_t i = 0; i < injector_count; i++) {
    injector_config.injector[i].injectorID = i + 1;
    injector_config.injector[i].volume = 10;
    injector_config.injector[i].interval = 60;
    injector_config.injector[i].enable = 1;
  }
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
/**
 * @fn void init_main_sys_state()
 * @brief 系统状态初始化函数,设置系统状态的初始值
 *
 */
void init_main_sys_state(void) {
  // 初始化系统状态
  main_sys_status.running_mode = SYS_MODE_STARTING;
  main_sys_status.sys_warning = SYS_RUNNING;
  main_sys_status.sys_start = 0;
  main_sys_status.p_model.point = motor_loc_list[0];
  main_sys_status.q_model.point = motor_loc_list[0];
  main_sys_status.q_model.sen = 0;
  main_sys_status.q_model.dir = 1;
  main_sys_status.is_Monitor = 1;
  main_sys_status.is_Busy = 1;

  uint8_t tag;
  for (tag = 0; tag < injector_count; tag++) {
    main_sys_status.injector[tag].injector_id = tag + 1;
    main_sys_status.injector[tag].volume = 0;
    main_sys_status.injector[tag].interval = 0;
    main_sys_status.injector[tag].executionTime = 0;
    main_sys_status.injector[tag].executionVol = 0;
    main_sys_status.injector[tag].injectRequest = 0;
    main_sys_status.injector[tag].task_status = 0;
    main_sys_status.injector[tag].execution_performance = 0;
    main_sys_status.injector[tag].task_volume = 0;
    main_sys_status.injector[tag].enable = 0;
  }
}

/**
 * @fn void init_task_object_list()
 * @brief 任务对象列表初始化函数，设置任务对象列表的初始值
 *
 */
void init_task_object_list(void) {
  // 初始化任务对象列表
  task_object_list[0].task_status = 0;
  task_object_list[0].inject_id = 0;
  task_object_list[0].val = 0;
  task_object_list[1].task_status = 0;
  task_object_list[1].inject_id = 0;
  task_object_list[1].val = 0;
  task_object_list[2].task_status = 0;
  task_object_list[2].inject_id = 0;
  task_object_list[2].val = 0;
  task_object_list[3].task_status = 0;
  task_object_list[3].inject_id = 0;
  task_object_list[3].val = 0;
  task_object_list[4].task_status = 0;
  task_object_list[4].inject_id = 0;
  task_object_list[4].val = 0;
}

/**
 * @fn void init_Q_motor_loc_list()
 * @brief Q电机位置列表初始化函数，设置Q电机位置列表的初始值
 *
 */
void init_Q_motor_loc_list(void) {
  // 初始化Q电机位置列表
  motor_loc_list[0].id = 0;
  motor_loc_list[0].deg = 0;
  motor_loc_list[0].pipe = 2;
  motor_loc_list[1].id = 1;
  motor_loc_list[1].deg = 90;
  motor_loc_list[1].pipe = 0;
  motor_loc_list[2].id = 2;
  motor_loc_list[2].deg = 180;
  motor_loc_list[2].pipe = 1;
  motor_loc_list[3].id = 3;
  motor_loc_list[3].deg = 270;
  motor_loc_list[3].pipe = 0;
  motor_loc_list[4].id = 4;
  motor_loc_list[4].deg = 360;
  motor_loc_list[4].pipe = 2;
  motor_loc_list[5].id = 5;
  motor_loc_list[5].deg = 360 + 90;
  motor_loc_list[5].pipe = 0;
  motor_loc_list[6].id = 6;
  motor_loc_list[6].deg = 360 + 180;
  motor_loc_list[6].pipe = 1;
  motor_loc_list[7].id = 7;
  motor_loc_list[7].deg = 360 + 270;
  motor_loc_list[7].pipe = 0;
  motor_loc_list[8].id = 8;
  motor_loc_list[8].deg = 720;
  motor_loc_list[8].pipe = 2;
  motor_loc_list[9].id = 9;
  motor_loc_list[9].deg = 720 + 90;
  motor_loc_list[9].pipe = 0;
  motor_loc_list[10].id = 10;
  motor_loc_list[10].deg = 720 + 180;
  motor_loc_list[10].pipe = 1;
  motor_loc_list[11].id = 11;
  motor_loc_list[11].deg = 720 + 270;
  motor_loc_list[11].pipe = 0;
}

/**
 * @fn void init_P_motor_loc_list()
 * @brief P电机位置列表初始化函数，设置P电机位置列表的初始值
 *
 */
void init_P_motor_loc_list(void) {
  // 初始化P电机位置列表
  motor_loc_list_p[0].id = 0;
  motor_loc_list_p[0].deg = 0;
  motor_loc_list_p[0].pipe = 0;
  motor_loc_list_p[1].id = 1;
  motor_loc_list_p[1].deg = 45;
  motor_loc_list_p[1].pipe = 2;
  motor_loc_list_p[2].id = 2;
  motor_loc_list_p[2].deg = 90;
  motor_loc_list_p[2].pipe = 0;
  motor_loc_list_p[3].id = 3;
  motor_loc_list_p[3].deg = 135;
  motor_loc_list_p[3].pipe = 1;
  motor_loc_list_p[4].id = 4;
  motor_loc_list_p[4].deg = 180;
  motor_loc_list_p[4].pipe = 0;
  motor_loc_list_p[5].id = 5;
  motor_loc_list_p[5].deg = 225;
  motor_loc_list_p[5].pipe = 2;
  motor_loc_list_p[6].id = 6;
  motor_loc_list_p[6].deg = 270;
  motor_loc_list_p[6].pipe = 0;
  motor_loc_list_p[7].id = 7;
  motor_loc_list_p[7].deg = 315;
  motor_loc_list_p[7].pipe = 1;
  motor_loc_list_p[8].id = 8;
  motor_loc_list_p[8].deg = 360;
  motor_loc_list_p[8].pipe = 0;
  motor_loc_list_p[9].id = 9;
  motor_loc_list_p[9].deg = 405;
  motor_loc_list_p[9].pipe = 2;
  motor_loc_list_p[10].id = 10;
  motor_loc_list_p[10].deg = 450;
  motor_loc_list_p[10].pipe = 0;
  motor_loc_list_p[11].id = 11;
  motor_loc_list_p[11].deg = 495;
  motor_loc_list_p[11].pipe = 1;
}