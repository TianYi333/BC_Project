/*
 * syncif.h
 *
 *  Created on: Oct 16, 2025
 *      Author: 28038
 */

#ifndef INC_SYNCIF_H_
#define INC_SYNCIF_H_

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "running_logic.h"
#include "mbReg.h"



#define APP_DB_VERSION         33


#define regID_IsRunning        200
#define regID_DateTime         202
#define regID_WorkMode         201
#define regID_Injector         300
#define regID_SlaveID          206
#define regID_BaudRate         207
#define regID_MaxMotorSpeed    209
#define regID_BaseMotorSpeed   211

#define syncif_task_stk_size     1024*8
#define syncif_task_prio         osPriorityNormal3




/**
 * 定义全局配置文件
 */

typedef struct main_config {
    // Modbus从站ID
    uint16_t slaveID;
    // 串口波特率
    uint32_t BaudRate;


    // 通信方式
    uint8_t commMode;
    // 工作模式
    uint8_t workMode;
    // 设备SN
    char deviceSN[16];
    // IP地址    
    char ipAddress[16];
    // 子网掩码    
    char subnetMask[16];
    // 网关地址    
    char gateway[16];
    // 服务器地址
    char serverAddress[16];
    // 服务器端口
    uint16_t serverPort;
    // 上位机状态
    uint8_t hostStatus;
    // 设备状态
    uint8_t deviceStatus;
} _MAIN_CONFIG;


/**
 * 注油点配置
 */
typedef struct point_config {
    // 注油点ID
    uint16_t injectorID;
    // 注油量，单位为毫升
    uint32_t volume;
    // 注油动作的间隔时间，单位为秒
    uint64_t interval;
    // 注油点开关
    uint8_t enable;
} _POINT_CONFIG;


/**
 * 注油器配置
 */
typedef struct injector_config {
    // 电机最大速度
    uint16_t maxMotorSpeed;
    // 电机基准速度
    uint16_t baseMotorSpeed;
    // 注油点配置数组
    _POINT_CONFIG injector[injector_count];
} _INJECTOR_CONFIG;

/**
 * 主要系统配置结构体实例
 * 该结构体存储了系统的各种配置参数，包括Modbus从站ID    、串口波特率、电机速度、通信方式、工作模式、设备SN、网络配置等。
 */
extern __attribute__((section("._DTC_FSRAM"))) _MAIN_CONFIG main_config;

/**
 * 注油点配置结构体数组
 * 该数组存储了多个注油点的配置参数，包括注油点ID、注油量、注油动作的间隔时间和开关状态。通过将该数组放置在特定的内存区域（._DTC_FSRAM），可以确保在系统重启后这些配置参数能够被保留和恢复，从而实现系统的持久化配置管理。
 */
extern __attribute__((section("._DTC_FSRAM"))) _INJECTOR_CONFIG injector_config;


/**
 * 数据同步的任务句柄
 */

extern TaskHandle_t syncif_task_handler;
extern TaskHandle_t syncif_task_handler_cycle;
/**
 * @fn void start_syncif_task(void)
 * @brief 数据同步接口任务
 *
 */
void start_syncif_task();

/**
 * @fn void syncif_task()
 * @brief 数据同步任务主函数
 *
 */
void syncif_task();
/**
 * @fn void sync_from_sysdb()
 * @brief 从数据库读取数据
 *
 */
void sync_from_sysdb();
/**
 * @fn void sync_to_sysdb()
 * @brief 同步数据到数据库
 *
 */
void sync_to_sysdb();
/**
 * @fn void sync_to_modbus()
 * @brief 同步数据到modbus寄存器数组
 *
 */
void sync_to_modbus();
/**
 * @fn void sync_from_modbus()
 * @brief 从modbus寄存器数组同步数据
 *
 */
void sync_from_modbus(_MB_REG mbMsg);
/**
 * @fn void sync_injector_timers()
 * @brief 同步定时器状态
 *
 */
void sync_injector_timers();
/**
 * @fn void syncif_task_cycle()
 * @brief 循环同步的任务函数
 *
 */
void syncif_task_cycle();

/**
 * @fn void deinit_sysdb()
 * @brief 反初始化系统数据库
 *
 */
void deinit_sysdb();
/**
 * @brief 同步配置文件
 */
void sync_config_file();

/**
 * @fn void init_sys_db()
 * @brief 数据库的初始化函数
 *
 */
int8_t init_sys_db();

void save_injector_config(_INJECTOR_CONFIG *cfg);

#endif /* INC_SYNCIF_H_ */
