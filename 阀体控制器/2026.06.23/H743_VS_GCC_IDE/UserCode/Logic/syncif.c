/*
 * syncif.c
 *
 *  Created on: Oct 16, 2025
 *      Author: 28038
 */

#include "syncif.h"
#include "Msg_Queue.h"
#include "cmsis_os.h"
#include "fal.h"
#include "flashdb.h"
#include "log.h"
#include "mb.h"
#include "rtc_clock.h"
#include "running_logic.h"
#include "semphr.h"
#include "shell_port.h"
#include "task.h"


/**
 * 数据同步的任务句柄
 */
TaskHandle_t syncif_task_handler;

/**
 * 循环同步的任务句柄
 */
TaskHandle_t syncif_task_handler_cycle;

/**
 * 数据同步任务参数
 */
const osThreadAttr_t syncif_task_attributes = {
    .name = "syncifTask",
    .stack_size = syncif_task_stk_size,
    .priority = syncif_task_prio,
};

/**
 * 系统配置结构体实例
 * 该结构体存储了系统的各种配置参数，包括Modbus从站ID
 * 、串口波特率、电机速度、通信方式、工作模式、设备SN、网络配置等。
 * 通过将该结构体放置在特定的内存区域（._DTC_FSRAM），可以确保在系统重启后这些配置参数能够被保留和恢复，从而实现系统的持久化配置管理。
 */
__attribute__((section("._DTC_FSRAM"))) _MAIN_CONFIG main_config;

/**
 * 注油点配置结构体数组
 * 该数组存储了多个注油点的配置参数，包括注油点ID、注油量、注油动作的间隔时间和开关状态。通过将该数组放置在特定的内存区域（._DTC_FSRAM），可以确保在系统重启后这些配置参数能够被保留和恢复，从而实现系统的持久化配置管理。
 */
__attribute__((section("._DTC_FSRAM"))) _INJECTOR_CONFIG injector_config;

/**
 * @fn void start_syncif_task(void)
 * @brief 数据同步接口任务
 *
 */
void start_syncif_task() {
  syncif_task_handler = osThreadNew(syncif_task, NULL, &syncif_task_attributes);
}

void start_syncif_cycle_task() {
  syncif_task_handler_cycle =
      osThreadNew(syncif_task_cycle, NULL, &syncif_task_attributes);
}

void lock(fdb_db_t db) {
  // taskENTER_CRITICAL(); /* 进入临界区 */
  // vTaskSuspendAll();
  //osMutexAcquire(flash_kv_mutex, osWaitForever);
}

void unlock(fdb_db_t db) {
  // taskEXIT_CRITICAL(); /* 退出临界区 */
  // xTaskResumeAll();
  //osMutexRelease(flash_kv_mutex);
}

uint16_t boot_count = 1;

uint32_t boot_time = 0;

/* default KV nodes */
static struct fdb_default_kv_node default_kv_table[] = {
    {"mfrs", "borsch", 0},
    {"device_id", "1234567890kv", 0},
    {"boot_count", &boot_count, sizeof(boot_count)},
    {"boot_time", &boot_time, sizeof(boot_time)},
    {"main_config", &main_config, sizeof(main_config)},
    {"injector_config", &injector_config, sizeof(injector_config)},
    {"version", "1.0.0", 0}};

/* KVDB object */
static struct fdb_kvdb kvdb = {0};

/**
 * @fn void init_sys_db()
 * @brief 数据库的初始化函数
 *
 */
int8_t init_sys_db() {

  //fal_init();
  fdb_err_t result;
  struct fdb_default_kv default_kv;
  default_kv.kvs = default_kv_table;
  default_kv.num = sizeof(default_kv_table) / sizeof(default_kv_table[0]);
  kvdb.ver_num = APP_DB_VERSION;
  fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_LOCK, (void *)lock);
  fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_UNLOCK, (void *)unlock);
  /* 初始化KVDB数据库，指定数据库名称、存储路径、默认KV节点等参数 */
  result = fdb_kvdb_init(&kvdb, "sysdb", "ef_kvdb1", &default_kv, NULL);
  if (result != FDB_NO_ERR) {
    return -1;
  }
  return 1;
}

/**
 * @fn void syncif_task()
 * @brief 数据同步任务主函数
 *
 */
void syncif_task() {

  /**
   * 初始化阶段要进行MB寄存器的初始化的数据归零
   */

  _MB_REG mb_Msg;
  //init_sys_db();
  sync_config_file();
  injector_timer_init();
  sync_from_sysdb();
  start_syncif_cycle_task();
  while (1) {
    if (xQueueReceive(QUEUE_CONN, &mb_Msg, 200) == pdPASS) {
      taskENTER_CRITICAL();
      sync_from_modbus(mb_Msg);
      taskEXIT_CRITICAL();
    } else {
      taskENTER_CRITICAL();
      sync_to_modbus();
      taskEXIT_CRITICAL();
    }
  }
}
/**
 * @fn void syncif_task_cycle()
 * @brief 循环同步的任务函数
 *
 */
void syncif_task_cycle() {
  while (1) {
    vTaskDelay(300);

    sync_injector_timers();
  }
}
/**
 * @fn void sync_injector_timers()
 * @brief 同步定时器状态
 *
 */
void sync_injector_timers() {
  uint8_t tag;
  if (main_sys_status.sys_start == 200) {
    for (tag = 0; tag < injector_count; tag++) {
      if (main_sys_status.injector[tag].interval > 0) {
        uint32_t time = main_sys_status.injector[tag].interval;
        if (xTimerGetPeriod(xTimeHandle[tag]) != time * 1000) {
          xTimerChangePeriod(xTimeHandle[tag], time * 1000, 1000);
        }
        if (xTimerIsTimerActive(xTimeHandle[tag]) == pdFALSE) {
          /* xTimer is active, do something. */
          xTimerStart(xTimeHandle[tag], 1000);
          logInfo("xTimerStart: %d ", tag);
        }
        TickType_t xRemainingTime = 0;
        xRemainingTime = xTimerGetExpiryTime(xTimeHandle[tag]);
        if (xRemainingTime != 0) {
          xRemainingTime -= xTaskGetTickCount();
        }
        main_sys_status.injector[tag].executionTime =
            (uint16_t)(xRemainingTime / 1000);
      }
    }
  } else {

    // for (tag = 0; tag < injector_count; tag++) {
    //   xTimerStop(xTimeHandle[tag], 1000);
    //   if (main_sys_status.injector[tag].interval > 0) {
    //     uint32_t time = main_sys_status.injector[tag].interval;
    //     if (xTimerGetPeriod(xTimeHandle[tag]) != time * 1000) {
    //       xTimerChangePeriod(xTimeHandle[tag], time * 1000, 1000);
    //     }
    //     if (xTimerIsTimerActive(xTimeHandle[tag]) != pdFALSE) {
    //       /* xTimer is active, do something. */
    //       xTimerStop(xTimeHandle[tag], 1000);
    //     }
    //   }
    // }
  }
}

/**
 * @fn void sync_from_sysdb()
 * @brief 从数据库读取数据
 *
 */
void sync_from_sysdb() {
  struct fdb_blob blob;
  fdb_kv_get_blob(&kvdb, "main_config",
                  fdb_blob_make(&blob, &main_config, sizeof(main_config)));
  fdb_kv_get_blob(
      &kvdb, "injector_config",
      fdb_blob_make(&blob, &injector_config, sizeof(injector_config)));
}

/**
 * @fn void sync_to_sysdb()
 * @brief 同步数据到数据库
 *
 */
void sync_to_sysdb() {
  struct fdb_blob blob;
  fdb_kv_set_blob(&kvdb, "main_config",
                  fdb_blob_make(&blob, &main_config, sizeof(main_config)));
  fdb_kv_set_blob(
      &kvdb, "injector_config",
      fdb_blob_make(&blob, &injector_config, sizeof(injector_config)));
}
/**
 * @brief 同步配置文件
 */
void sync_config_file() {
  uint8_t tag = 0;
  for (tag = 0; tag < injector_count; tag++) {
    main_sys_status.injector[tag].interval =injector_config.injector[tag].interval;
    main_sys_status.injector[tag].volume = injector_config.injector[tag].volume;
    main_sys_status.injector[tag].enable = injector_config.injector[tag].enable;
  }
}

/**
 * @fn void sync_to_modbus()
 * @brief 同步数据到modbus寄存器数组
 *
 */
void sync_to_modbus() {

  // 先同步配置文件
  sync_config_file();

  usRegHoldingBuf[regID_IsRunning] = main_sys_status.sys_start;
  usRegHoldingBuf[regID_WorkMode] = main_config.workMode;
  uint64_t Utime = Time_To_Unix() * 1000;
  usRegHoldingBuf[regID_DateTime] = Utime >> 48;
  usRegHoldingBuf[regID_DateTime + 1] = Utime >> 32;
  usRegHoldingBuf[regID_DateTime + 2] = Utime >> 16;
  usRegHoldingBuf[regID_DateTime + 3] = Utime >> 0;

  usRegHoldingBuf[regID_SlaveID] = main_config.slaveID;
  usRegHoldingBuf[regID_BaudRate] = main_config.BaudRate >> 16;
  usRegHoldingBuf[regID_BaudRate + 1] = main_config.BaudRate >> 0;
  usRegHoldingBuf[regID_MaxMotorSpeed] = injector_config.maxMotorSpeed >> 16;
  usRegHoldingBuf[regID_MaxMotorSpeed + 1] = injector_config.baseMotorSpeed;
  usRegHoldingBuf[regID_BaseMotorSpeed] = injector_config.baseMotorSpeed >> 16;
  usRegHoldingBuf[regID_BaseMotorSpeed + 1] = injector_config.baseMotorSpeed;

  uint8_t tag = 0;
  for (tag = 0; tag < (sizeof(main_sys_status.injector) /
                       sizeof(main_sys_status.injector[0]));
       tag++) {
    if (main_sys_status.injector[tag].enable == 0) {
      // 润滑点配置的间隔时间需要特殊处理，因为它是一个64位的数，要分成四个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 0] =
          main_sys_status.injector[tag].interval >> 48;
      usRegHoldingBuf[300 + (tag * 20) + 1] =
          main_sys_status.injector[tag].interval >> 32;
      usRegHoldingBuf[300 + (tag * 20) + 2] =
          main_sys_status.injector[tag].interval >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 3] =
          main_sys_status.injector[tag].interval >> 0;
      // 润滑点配置的注油量需要特殊处理，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 4] =
          main_sys_status.injector[tag].volume >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 5] =
          main_sys_status.injector[tag].volume >> 0;
      // 润滑点注油请求
      usRegHoldingBuf[300 + (tag * 20) + 6] = 0;
      // 润滑点的待机进度时间，因为它是一个64位的数，要分成四个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 7] = 0;
      usRegHoldingBuf[300 + (tag * 20) + 8] = 0;
      usRegHoldingBuf[300 + (tag * 20) + 9] = 0;
      usRegHoldingBuf[300 + (tag * 20) + 10] = 0;
      // 润滑点的执行注油进度，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 11] = 0;
      usRegHoldingBuf[300 + (tag * 20) + 12] = 0;
      // 润滑点的执行结果
      usRegHoldingBuf[300 + (tag * 20) + 13] = 0;
      // 润滑点的执行性能，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 14] = 0;
      usRegHoldingBuf[300 + (tag * 20) + 15] = 0;
      // 润滑点的任务要求注油量，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 16] = 0;
      usRegHoldingBuf[300 + (tag * 20) + 17] = 0;
      // 润滑点的开关状态
      usRegHoldingBuf[300 + (tag * 20) + 18] =
          main_sys_status.injector[tag].enable;
    } else {
      // 润滑点配置的间隔时间需要特殊处理，因为它是一个64位的数，要分成四个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 0] =
          main_sys_status.injector[tag].interval >> 48;
      usRegHoldingBuf[300 + (tag * 20) + 1] =
          main_sys_status.injector[tag].interval >> 32;
      usRegHoldingBuf[300 + (tag * 20) + 2] =
          main_sys_status.injector[tag].interval >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 3] =
          main_sys_status.injector[tag].interval >> 0;
      // 润滑点配置的注油量需要特殊处理，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 4] =
          main_sys_status.injector[tag].volume >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 5] =
          main_sys_status.injector[tag].volume >> 0;
      // 润滑点注油请求
      usRegHoldingBuf[300 + (tag * 20) + 6] =
          main_sys_status.injector[tag].injectRequest;
      // 润滑点的待机进度时间，因为它是一个64位的数，要分成四个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 7] =
          main_sys_status.injector[tag].executionTime >> 48;
      usRegHoldingBuf[300 + (tag * 20) + 8] =
          main_sys_status.injector[tag].executionTime >> 32;
      usRegHoldingBuf[300 + (tag * 20) + 9] =
          main_sys_status.injector[tag].executionTime >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 10] =
          main_sys_status.injector[tag].executionTime >> 0;
      // 润滑点的执行注油进度，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 11] =
          main_sys_status.injector[tag].executionVol >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 12] =
          main_sys_status.injector[tag].executionVol >> 0;
      // 润滑点的执行结果
      usRegHoldingBuf[300 + (tag * 20) + 13] =
          main_sys_status.injector[tag].task_status;
      // 润滑点的执行性能，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 14] =
          main_sys_status.injector[tag].execution_performance >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 15] =
          main_sys_status.injector[tag].execution_performance >> 0;
      // 润滑点的任务要求注油量，因为它是一个32位的数，要分成两个16位的寄存器来存储
      usRegHoldingBuf[300 + (tag * 20) + 16] =
          main_sys_status.injector[tag].task_volume >> 16;
      usRegHoldingBuf[300 + (tag * 20) + 17] =
          main_sys_status.injector[tag].task_volume >> 0;
      // 润滑点的开关状态
      usRegHoldingBuf[300 + (tag * 20) + 18] =
          main_sys_status.injector[tag].enable;
    }
  }
}
/**
 * @fn void sync_from_modbus()
 * @brief 从modbus寄存器数组同步数据
 *
 */
void sync_from_modbus(_MB_REG mbMsg) {
  // 主配置变更的信号值
  uint8_t main_config_changed = 0;
  // 注油点配置变更的信号值
  uint8_t injector_config_changed = 0;
  // 判断系统启动寄存器是否在修改范围内
  if (mbMsg.startAddr <= regID_IsRunning &&
      regID_IsRunning <= (mbMsg.startAddr + mbMsg.len)) {
    main_sys_status.sys_start = usRegHoldingBuf[regID_IsRunning];
  }

  // 判断时间寄存器是否在修改范围内

  if (mbMsg.startAddr <= regID_DateTime &&
      regID_DateTime + 3 <= (mbMsg.startAddr + mbMsg.len)) {

    uint64_t Utime = 0;

    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime] << 48;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 1] << 32;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 2] << 16;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 3] << 0;

    if ((Utime > 1735660800000) && (Utime < 2366812800000)) {
      Unix_To_Time(Utime / 1000);
    }
  }
  // 判断工作模式寄存器是否在修改范围内
  if (mbMsg.startAddr <= regID_WorkMode &&
      regID_WorkMode <= (mbMsg.startAddr + mbMsg.len)) {
    if (main_config.workMode != usRegHoldingBuf[regID_WorkMode]) {
      main_config.workMode = usRegHoldingBuf[regID_WorkMode];
      main_config_changed++;
    }
  }
  // 判断从站ID寄存器是否在修改范围内
  if (mbMsg.startAddr <= regID_SlaveID &&
      regID_SlaveID <= (mbMsg.startAddr + mbMsg.len)) {
    if (main_config.slaveID != usRegHoldingBuf[regID_SlaveID]) {
      main_config.slaveID = usRegHoldingBuf[regID_SlaveID];
      main_config_changed++;
    }
  }
  // 判断波特率寄存器是否在修改范围内
  if (mbMsg.startAddr <= regID_BaudRate &&
      regID_BaudRate + 1 <= (mbMsg.startAddr + mbMsg.len)) {
    uint32_t newBaudRate = (usRegHoldingBuf[regID_BaudRate] << 16) |
                           usRegHoldingBuf[regID_BaudRate + 1];
    // 需要增加输入值的合法性校验，波特率只能是常见的几种值
    if (newBaudRate != 9600 && newBaudRate != 19200 && newBaudRate != 38400 &&
        newBaudRate != 57600 && newBaudRate != 115200) {
      logError("Invalid BaudRate value: %u", newBaudRate);
      return;
    }
    if (main_config.BaudRate != newBaudRate) {
      main_config.BaudRate = newBaudRate;
      main_config_changed++;
    }
  }
  // 判断电机最大速度寄存器是否在修改范围内
  if (mbMsg.startAddr <= regID_MaxMotorSpeed &&
      regID_MaxMotorSpeed + 1 <= (mbMsg.startAddr + mbMsg.len)) {
    uint32_t maxMotorSpeed = (usRegHoldingBuf[regID_MaxMotorSpeed] << 16) |
                             usRegHoldingBuf[regID_MaxMotorSpeed + 1];
    if (injector_config.maxMotorSpeed != maxMotorSpeed) {
      // 需要增加输入值的合法性校验，最大速度不能超过电机的物理限制
      injector_config.maxMotorSpeed = maxMotorSpeed;
      injector_config_changed++;
    }
  }
  // 判断电机基准速度寄存器是否在修改范围内
  if (mbMsg.startAddr <= regID_BaseMotorSpeed &&
      regID_BaseMotorSpeed + 1 <= (mbMsg.startAddr + mbMsg.len)) {
    uint32_t baseMotorSpeed = (usRegHoldingBuf[regID_BaseMotorSpeed] << 16) |
                              usRegHoldingBuf[regID_BaseMotorSpeed + 1];
    if (injector_config.baseMotorSpeed != baseMotorSpeed) {
      // 需要增加输入值的合法性校验，基准速度不能超过电机的物理限制
      injector_config.baseMotorSpeed = baseMotorSpeed;
      injector_config_changed++;
    }
  }

  // 判断注油点配置寄存器是否在修改范围内
  if (regID_Injector <= mbMsg.startAddr &&
      (300 + (injector_count * 20) - 1) >= (mbMsg.startAddr + mbMsg.len)) {
    uint8_t tag = 0;
    for (tag = 0; tag < (sizeof(injector_config.injector) /
                         sizeof(injector_config.injector[0]));
         tag++) {
      // 定义该注油点的寄存器起始地址
      uint16_t injector_reg_start = 300 + (tag * 20);

      // 判断注油点配置的间隔时间寄存器是否在修改范围内
      if (mbMsg.startAddr <= injector_reg_start &&
          injector_reg_start + 3 <= (mbMsg.startAddr + mbMsg.len)) {
        // 从Modbus寄存器中读取新的间隔时间值
        uint64_t new_interval = 0;
        new_interval |= (uint64_t)usRegHoldingBuf[injector_reg_start] << 48;
        new_interval |= (uint64_t)usRegHoldingBuf[injector_reg_start + 1] << 32;
        new_interval |= (uint64_t)usRegHoldingBuf[injector_reg_start + 2] << 16;
        new_interval |= (uint64_t)usRegHoldingBuf[injector_reg_start + 3] << 0;
        // 如果新的间隔时间值与当前系统状态中的值不同，则更新系统状态中的值，并标记主配置发生了变更
        if (injector_config.injector[tag].interval != new_interval) {
          // 需要增加输入值的合法性校验，间隔时间不能超过系统的合理范围(最长设置时间为40-天)
          if (new_interval > 5 && new_interval < 3456000) {
            injector_config.injector[tag].interval = new_interval;
            injector_config_changed++;
          } else {
            // 如果输入值不合法，可以选择将其重置为默认值，或者保持原有值不变，并记录日志以便后续分析
            logError("Invalid interval value for injector %d: %llu", tag + 1,
                     new_interval);
          }
        }
      }
      // 判断注油点配置的注油量寄存器是否在修改范围内
      if (mbMsg.startAddr <= injector_reg_start + 4 &&
          injector_reg_start + 4 + 1 <= (mbMsg.startAddr + mbMsg.len)) {
        // 从Modbus寄存器中读取新的注油量值
        uint32_t new_volume = 0;
        new_volume |= (uint32_t)usRegHoldingBuf[injector_reg_start + 4] << 16;
        new_volume |= (uint32_t)usRegHoldingBuf[injector_reg_start + 5] << 0;
        // 如果新的注油量值与当前系统状态中的值不同，则更新系统状态中的值，并标记主配置发生了变更
        if (injector_config.injector[tag].volume != new_volume) {
          // 需要增加输入值的合法性校验，注油量不能超过系统的合理范围
          if (new_volume > 0 && new_volume < 1000) {
            injector_config.injector[tag].volume = new_volume;
            injector_config_changed++;
          } else {
            // 如果输入值不合法，可以选择将其重置为默认值，或者保持原有值不变，并记录日志以便后续分析
            logError("Invalid volume value for injector %d: %u", tag + 1,
                     new_volume);
          }
        }
      }
      // 判断注油点配置的开关状态寄存器是否在修改范围内
      if (mbMsg.startAddr <= injector_reg_start + 18 &&
          injector_reg_start + 18 <= (mbMsg.startAddr + mbMsg.len)) {
        // 从Modbus寄存器中读取新的开关状态值
        uint16_t new_enable = 0;
        new_enable |= (uint16_t)usRegHoldingBuf[injector_reg_start + 18];
        // 如果新的开关状态与当前系统状态中的值不同，则更新系统状态中的值，并标记主配置发生了变更
        if (injector_config.injector[tag].enable != new_enable) {
          injector_config.injector[tag].enable = new_enable;
          injector_config_changed++;
        }
      }
      // 判断注油结果寄存器是否在修改范围内
      if (mbMsg.startAddr <= injector_reg_start + 13 &&
          injector_reg_start + 13 <= (mbMsg.startAddr + mbMsg.len)) {
        // 从Modbus寄存器中读取新的注油结果值
        uint16_t new_task_status = 0;
        new_task_status |= (uint16_t)usRegHoldingBuf[injector_reg_start + 13];
        // 如果新的注油结果值与当前系统状态中的值不同，则更新系统状态中的值，并标记主配置发生了变更
        if (main_sys_status.injector[tag].task_status != new_task_status) {
          main_sys_status.injector[tag].task_status = new_task_status;
        }
      }

      // 判断注油请求寄存器是否在修改范围内
      if (mbMsg.startAddr <= injector_reg_start + 6 &&
          injector_reg_start + 6 <= (mbMsg.startAddr + mbMsg.len)) {
        // 从Modbus寄存器中读取新的注油请求值
        uint16_t new_injectRequest = 0;
        new_injectRequest |= (uint16_t)usRegHoldingBuf[injector_reg_start + 6];
        // 如果新的注油请求值与当前系统状态中的值不同，则更新系统状态中的值，并标记主配置发生了变更
        if (main_sys_status.injector[tag].injectRequest != new_injectRequest) {
          main_sys_status.injector[tag].injectRequest = new_injectRequest;
        }
      }


      // 判断注油点配置的任务要求注油量寄存器是否在修改范围内
      if (mbMsg.startAddr <= injector_reg_start + 16 &&
          injector_reg_start + 16 + 1 <= (mbMsg.startAddr + mbMsg.len)) {
        // 从Modbus寄存器中读取新的任务要求注油量值
        uint32_t new_task_volume = 0;
        new_task_volume |= (uint32_t)usRegHoldingBuf[injector_reg_start + 16]
                           << 16;
        new_task_volume |= (uint32_t)usRegHoldingBuf[injector_reg_start + 17]
                           << 0;
        // 如果新的任务要求注油量大于零，在任务列表中添加任务，并将寄存器值修改为零。
        if (new_task_volume > 0 && main_sys_status.is_Busy == 0 &&
            main_sys_status.injector[tag].enable > 0) {
          task_object_list[0].task_status = 1;
          task_object_list[0].inject_id = tag + 1;
          task_object_list[0].val = new_task_volume;
          // 将寄存器值修改为零，表示任务已经被系统接受
          usRegHoldingBuf[injector_reg_start + 16] = 0;
          usRegHoldingBuf[injector_reg_start + 17] = 0;
        }
      }
    }
  }

  // 判断是否发生配置变更，如果有变更则同步到数据库
  if (main_config_changed > 0) {
    struct fdb_blob blob;
    fdb_kv_set_blob(&kvdb, "main_config",
                    fdb_blob_make(&blob, &main_config, sizeof(main_config)));
  }
  if (injector_config_changed > 0) {
    struct fdb_blob blob;
    fdb_kv_set_blob(
        &kvdb, "injector_config",
        fdb_blob_make(&blob, &injector_config, sizeof(injector_config)));
    // 配置发生变更后同步到系统
    sync_config_file();
    sync_injector_timers();
  }
}

/**
 * @fn void deinit_sysdb()
 * @brief 反初始化系统数据库
 *
 */
void deinit_sysdb() { fdb_kvdb_deinit(&kvdb); }

/**
 * @brief 完全格式化并重置 FlashDB（所有数据清零，恢复默认配置）
 */
int8_t flashdb_format_and_reset(void) {

  fal_partition_t part;

  fdb_kvdb_deinit(&kvdb);

  init_sys_db();

  logInfo("FlashDB format and reset OK");
  return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 formatdb, flashdb_format_and_reset,
                 "Format and reset FlashDB to factory default");

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 resetdb, deinit_sysdb,
                 "This is a command that reset system database.");

void SoftReset(void) {
  __set_FAULTMASK(1); // 关闭所有中断
  NVIC_SystemReset(); // 请求系统复位
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 reboot, SoftReset, "This is a command that reboot system.");

void print_sysdb() { fdb_kv_print(&kvdb); }

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 printdb, print_sysdb,
                 "This is a command that print database.");
