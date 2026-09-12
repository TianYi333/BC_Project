#ifndef INC_SYNCIF_H_
#define INC_SYNCIF_H_

#include "project_config.h"
#include "msg_queue.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "mbReg.h"
#include "main_logic.h"



#define APP_DB_VERSION         33

#define regID_DateTime         300

// 同步任务配置
#define syncif_task_stk_size     1024*8
#define syncif_task_prio         osPriorityNormal

/**
 * 全局统一系统配置，定义在main_logic.h
 * 原main_config/injector_config全部合并至SYS_CONFIG_T sys_cfg
 */
extern __attribute__((section("._DTC_FSRAM"))) SYS_CONFIG_T sys_cfg;

/**
 * 数据同步任务句柄
 */
extern TaskHandle_t Syncif_task_handler;

/**
 * @fn void start_syncif_task(void)
 * @brief 创建并启动数据同步任务
 */
void start_syncif_task();

/**
 * @fn void Syncif_task()
 * @brief 数据同步任务主循环
 * 200ms周期：处理上位机寄存器写入 + 周期刷新Modbus输出寄存器
 */
void Syncif_task();

/**
 * @fn void sync_from_sysdb()
 * @brief FlashDB加载全局sys_cfg配置
 */
void sync_from_sysdb();

/**
 * @fn void sync_to_sysdb()
 * @brief 将当前sys_cfg写入Flash持久保存
 */
void sync_to_sysdb();

/**
 * @fn void sync_to_modbus()
 * @brief 全局状态 + sys_cfg 填充Modbus保持寄存器
 * 严格遵循油泵Modbus协议浮点×100缩放、32bit高低地址规则
 */
void sync_to_modbus();

/**
 * @fn void sync_from_modbus(_MB_REG mbMsg)
 * @brief 上位写寄存器解析、参数范围校验、故障拦截
 * 参数变更自动调用sync_to_sysdb落盘
 */
void sync_from_modbus(_MB_REG mbMsg);

/**
 * @fn void sync_injector_timers()
 * @brief 注油业务已移除，仅空声明兼容旧工程调用
 */
void sync_injector_timers();

/**
 * @fn void deinit_sysdb()
 * @brief 反初始化FlashDB数据库
 */
void deinit_sysdb();

/**
 * @fn int8_t init_sys_db()
 * @brief FlashDB初始化，加载默认键值表
 */
int8_t init_sys_db();

/**
 * @brief 废弃注油配置保存接口，仅兼容编译，内部空实现
 */
void save_injector_config(void);

#endif /* INC_SYNCIF_H_ */