#include "syncif.h"
#include "main.h"
#include "cmsis_os.h"
#include "fal.h"
#include "flashdb.h"
#include "log.h"
#include "mb.h"
#include "rtc_clock.h"
#include "semphr.h"
#include "shell_port.h"
#include "task.h"
#include "main_logic.h"

// Modbus 浮点缩放、32位拼接工具宏
#define FLOAT_TO_REG(f)    ((int32_t)((f) * 100.0f))
#define REG_TO_FLOAT(r)    ((float)(r) / 100.0f)
#define PACK32(hi,lo)      (((uint32_t)(hi) << 16) | (uint16_t)(lo))
#define UNPACK32(val,hi,lo) do{hi = (uint16_t)((uint32_t)(val) >> 16); lo = (uint16_t)((uint32_t)(val) & 0xFFFF);}while(0)

// 前置声明，消除 Cfg_SetDefault 隐式声明警告
int Cfg_SetDefault(void);

extern osMutexId_t flash_kv_mutex;

/**
 * 数据同步的任务句柄
 */
TaskHandle_t Syncif_task_handler;

/**
 * 数据同步任务参数
 */
const osThreadAttr_t Syncif_task_attributes = {
    .name = "SyncifTask",
    .stack_size = syncif_task_stk_size,
    .priority = syncif_task_prio,
};

// 开机计数、开机时间KV全局变量
uint16_t boot_count = 1;
uint32_t boot_time = 0;

/* default KV nodes 键值表，移除废弃main_config/injector_config，仅保留sys_cfg */
static struct fdb_default_kv_node default_kv_table[] = {
    {"mfrs", "borsch", 0},
    {"device_id", "1234567890kv", 0},
    {"boot_count", &boot_count, sizeof(boot_count)},
    {"boot_time", &boot_time, sizeof(boot_time)},
    {"sys_cfg", &sys_cfg, sizeof(SYS_CONFIG_T)},
    {"version", "1.0.0", 0}};

/* KVDB对象 */
static struct fdb_kvdb kvdb = {0};

/**
 * @fn void start_syncif_task(void)
 * @brief 数据同步接口任务创建
 */
void start_syncif_task() {
  Syncif_task_handler = osThreadNew(Syncif_task, NULL, &Syncif_task_attributes);
}

/**
 * FlashDB互斥锁回调
 */
void lock(fdb_db_t db) {
  (void)db;
  if (flash_kv_mutex == NULL)
  {
      return;
  }
  osMutexAcquire(flash_kv_mutex, osWaitForever);
}

void unlock(fdb_db_t db) {
  (void)db;
  if (flash_kv_mutex == NULL)
  {
      return;
  }
  osMutexRelease(flash_kv_mutex);
}

/**
 * @fn int8_t init_sys_db()
 * @brief 数据库初始化
 */
int8_t init_sys_db() {
  fdb_err_t result;
  struct fdb_default_kv default_kv;
  default_kv.kvs = default_kv_table;
  default_kv.num = sizeof(default_kv_table) / sizeof(default_kv_table[0]);
  kvdb.ver_num = APP_DB_VERSION;
  fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_LOCK, (void *)lock);
  fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_UNLOCK, (void *)unlock);
  result = fdb_kvdb_init(&kvdb, "sysdb", "ef_kvdb1", &default_kv, NULL);
  if (result != FDB_NO_ERR) {
    return -1;
  }
  return 1;
}

/**
 * @fn void Syncif_task()
 * @brief 数据同步任务主函数
 * 200ms阻塞等待Modbus写队列，无写则周期刷新保持寄存器
 */
void Syncif_task() {
  _MB_REG mb_Msg;
  sync_from_sysdb();
  while (1) {
    if (xQueueReceive(QUEUE_CONN, &mb_Msg, pdMS_TO_TICKS(220)) == pdPASS) {
      sync_from_modbus(mb_Msg);
    } else {
      sync_to_modbus();
    }
  }
}

/**
 * @fn void sync_from_sysdb()
 * @brief 从FlashDB读取sys_cfg全局配置
 */
void sync_from_sysdb() {
  struct fdb_blob blob;
  fdb_kv_get_blob(&kvdb, "sys_cfg", fdb_blob_make(&blob, &sys_cfg, sizeof(SYS_CONFIG_T)));
}

/**
 * @fn void sync_to_sysdb()
 * @brief 将修改后的sys_cfg写入Flash持久化
 */
void sync_to_sysdb() {
  struct fdb_blob blob;
  fdb_kv_set_blob(&kvdb, "sys_cfg", fdb_blob_make(&blob, &sys_cfg, sizeof(SYS_CONFIG_T)));
}

/**
 * @fn void sync_to_modbus()
 * @brief 全局状态sys_tmp + sys_cfg 填充Modbus保持寄存器
 * 严格遵循油泵Modbus协议V1.0地址分区映射
 */
void sync_to_modbus() {
  _SYS_STATUS sys_tmp;
  if (SysStatus_ReadSnapshot(&sys_tmp) == 0) {
    return;
  }

  //====================分区1：实时运行数据 0000~0049 只读====================
  //0000-0001 当前实时压力 float×100
  int32_t p_scalar = FLOAT_TO_REG(sys_tmp.adc_pressure);
  UNPACK32(p_scalar, usRegHoldingBuf[0], usRegHoldingBuf[1]);
  //0002-0003 基准目标压力
  int32_t ref_scalar = FLOAT_TO_REG(sys_cfg.ref_pressure);
  UNPACK32(ref_scalar, usRegHoldingBuf[2], usRegHoldingBuf[3]);
  //0004-0005 电机实际转速 int32
  UNPACK32((int32_t)sys_tmp.motor_status.actual_speed, usRegHoldingBuf[4], usRegHoldingBuf[5]);
  //0006-0007 油液实时温度
  int32_t temp_scalar = FLOAT_TO_REG(sys_tmp.adc_oil_temp);
  UNPACK32(temp_scalar, usRegHoldingBuf[6], usRegHoldingBuf[7]);
  //0008-0009 油箱液位百分比
  int32_t liq_scalar = FLOAT_TO_REG(sys_tmp.liquid_level_pct);
  UNPACK32(liq_scalar, usRegHoldingBuf[8], usRegHoldingBuf[9]);

  usRegHoldingBuf[10] = sys_tmp.sys_start;                  //系统整机状态码
  usRegHoldingBuf[11] = sys_tmp.motor_status.state;        //电机软件状态
  usRegHoldingBuf[12] = sys_tmp.valve_state;                //泄压阀状态
  usRegHoldingBuf[13] = (sys_tmp.sys_fault_bit == 0) ? 1 : 0; //传感器综合状态
  usRegHoldingBuf[14] = sys_tmp.motor_status.err_code;     //伺服故障码
  usRegHoldingBuf[15] = sys_tmp.motor_status.status_word;  //伺服状态字
  usRegHoldingBuf[16] = sys_tmp.motor_status.comm_lost;    //电机通讯丢失标志

  //====================分区2：故障报警状态 0050~0099 只读====================
  usRegHoldingBuf[50] = sys_tmp.sys_fault_bit;  //直接透传完整故障位图！
  //【修改点】简化全局故障判断，不再重复计算，直接使用故障位图
  uint16_t global_fault = (sys_tmp.sys_fault_bit != 0U) ? 1U : 0U;
  usRegHoldingBuf[51] = global_fault;

  //====================分区3：用户控制参数 0100~0149 读写====================
  //0100-0101 最高保护压力
  int32_t max_p_scalar = FLOAT_TO_REG(sys_cfg.max_pressure);
  UNPACK32(max_p_scalar, usRegHoldingBuf[100], usRegHoldingBuf[101]);
  //0102-0103 电机最高转速
  UNPACK32((int32_t)sys_cfg.max_motor_speed, usRegHoldingBuf[102], usRegHoldingBuf[103]);
  //0104-0105 电机最低转速
  UNPACK32((int32_t)sys_cfg.min_motor_speed, usRegHoldingBuf[104], usRegHoldingBuf[105]);
  //0106-0107 油温停机阈值
  int32_t ot_scalar = FLOAT_TO_REG(sys_cfg.max_oil_temp);
  UNPACK32(ot_scalar, usRegHoldingBuf[106], usRegHoldingBuf[107]);
  //0108-0109 最低液位阈值
  int32_t liq_thr = FLOAT_TO_REG(sys_cfg.min_liquid_level);
  UNPACK32(liq_thr, usRegHoldingBuf[108], usRegHoldingBuf[109]);
  //0110-0111 压力回差
  int32_t hyst_scalar = FLOAT_TO_REG(sys_cfg.press_hysteresis);
  UNPACK32(hyst_scalar, usRegHoldingBuf[110], usRegHoldingBuf[111]);
  //0112-0113 超压安全裕量百分比
  int32_t margin_scalar = FLOAT_TO_REG(sys_cfg.overpress_margin);
  UNPACK32(margin_scalar, usRegHoldingBuf[112], usRegHoldingBuf[113]);

  //====================分区4 PID参数 0150~0169====================
  int32_t kp = FLOAT_TO_REG(sys_cfg.pid_kp);
  UNPACK32(kp, usRegHoldingBuf[150], usRegHoldingBuf[151]);
  int32_t ki = FLOAT_TO_REG(sys_cfg.pid_ki);
  UNPACK32(ki, usRegHoldingBuf[152], usRegHoldingBuf[153]);
  int32_t kd = FLOAT_TO_REG(sys_cfg.pid_kd);
  UNPACK32(kd, usRegHoldingBuf[154], usRegHoldingBuf[155]);

  //====================分区5 通讯配置 0170~0189====================
  usRegHoldingBuf[170] = sys_cfg.modbus_addr;
  usRegHoldingBuf[171] = sys_cfg.modbus_baud_sel;

  //====================分区6 远程控制 0200~0249====================
  usRegHoldingBuf[200] = sys_tmp.sys_start;       //0200 系统模式
  usRegHoldingBuf[201] = sys_tmp.valve_state;     //0201 泄压阀状态
  usRegHoldingBuf[202] = 0; //0202、0203 远程目标电机转速 32位无接口返回0
  usRegHoldingBuf[203] = 0;
  usRegHoldingBuf[204] = 0; //清除故障指令为写操作，只读返回0
  usRegHoldingBuf[205] = 0; //出厂复位码只读返回0
  
  // RTC时间 寄存器
  uint64_t Utime = Time_To_Unix() * 1000;
  usRegHoldingBuf[regID_DateTime]     = (uint16_t)(Utime >> 48);
  usRegHoldingBuf[regID_DateTime + 1] = (uint16_t)(Utime >> 32);
  usRegHoldingBuf[regID_DateTime + 2] = (uint16_t)(Utime >> 16);
  usRegHoldingBuf[regID_DateTime + 3] = (uint16_t)(Utime >> 0);
}

/**
 * @fn void sync_from_modbus()
 * @brief 上位写寄存器下发处理，支持锁定故障、手动复位、强制停止SETTING
 */
void sync_from_modbus(_MB_REG mbMsg) {
  uint16_t start = mbMsg.startAddr;
  uint16_t end = mbMsg.startAddr + mbMsg.len - 1;
  uint8_t param_changed = 0;

  // 读取当前全局状态
  _SYS_STATUS sys_tmp;
  SysStatus_ReadSnapshot(&sys_tmp);
  //【修改点】统一使用故障位图判断整机故障
  uint16_t global_fault = (sys_tmp.sys_fault_bit != 0U) ? 1U : 0U;

  //【重要修改】锁定故障判定：和main_logic逻辑完全一致
  // 锁定故障 = 油温过高 || 液位过低
  uint8_t lock_fault = 0;
  if( ((sys_tmp.sys_fault_bit & SYS_ERR_OIL_TEMP_OVER) != 0) ||
      ((sys_tmp.sys_fault_bit & SYS_ERR_LOW_LIQUID) != 0) )
  {
      lock_fault = 1;
  }

  //====================分区6 远程调试控制 0200~0249====================
  //0200 远程系统模式指令
  if(start <= 200 && end >= 200) {
    uint16_t cmd = usRegHoldingBuf[200];
    _SYS_STATUS write_tmp = sys_tmp;
    switch(cmd)
    {
        case 0:
            //无操作
            break;
        case 1: //强制停机 → 进入设置模式 SETTING
            write_tmp.sys_start = SYS_STATE_SETTING;
            SysStatus_WriteSnapshot(&write_tmp);
            LOG_Debug("Remote cmd: force stop, enter setting mode");
            break;
        case 5: //恢复自动 → 修改为：切至停机 STOP
            write_tmp.sys_start = SYS_STATE_STOP;
            SysStatus_WriteSnapshot(&write_tmp);
            LOG_Debug("Remote cmd: switch to stop mode");
            break;
        case 2: //加压
        case 3: //保压
        case 4: //泄压
            //【远程禁用加压/保压/泄压指令，直接丢弃】
            LOG_Debug("Remote pressure control cmd(2/3/4) disabled, ignore");
            break;
        default:
            //非法指令忽略
            break;
    }
  }
  //0201 远程泄压阀手动
  if(start <= 201 && end >= 201) {
    if(global_fault) return;
    uint16_t valve_cmd = usRegHoldingBuf[201];
    if(valve_cmd == 0 || valve_cmd == 1) {
      _SYS_STATUS write_tmp = sys_tmp;
      write_tmp.valve_state = valve_cmd;
      SysStatus_WriteSnapshot(&write_tmp);
    }
  }
  //0202、0203 远程目标电机转速【预留调试接口，暂不参与自动压力控制逻辑】
  if(start <= 202 && end >= 203) {
    //协议约束：系统存在故障拒绝写入
    if(global_fault) return;
    uint32_t raw = PACK32(usRegHoldingBuf[202], usRegHoldingBuf[203]);
    int32_t target_rpm = (int32_t)raw;
    //上下限保护
    if(target_rpm >= -(int32_t)sys_cfg.max_motor_speed && target_rpm <= (int32_t)sys_cfg.max_motor_speed)
    {

    }
  }
  //0204 手动复位故障指令
  if(start <= 204 && end >= 204) {
    if(usRegHoldingBuf[204] == 1)
    {
        _SYS_STATUS write_tmp = sys_tmp;
        if(lock_fault)
        {
            LOG_Debug("Reset failed: locked fault (oil temp / low liquid) exists");
        }
        else
        {
            write_tmp.fault_reset_req = 1;
            SysStatus_WriteSnapshot(&write_tmp);
            LOG_Debug("Host send fault reset command");
        }
    }
  }
  //0205 恢复出厂触发码
  if(start <= 205 && end >= 205) {
    if(usRegHoldingBuf[205] == 0x55AA) {
      Cfg_SetDefault();
      sync_to_sysdb();
      LOG_Debug("Restore factory config success");
    }
  }

  //====================分区3 用户控制参数 0100~0149====================
  //最高保护压力
  if(start <= 100 && end >= 101) {
    uint32_t raw = PACK32(usRegHoldingBuf[100], usRegHoldingBuf[101]);
    float val = REG_TO_FLOAT((int32_t)raw);
    if(val >= 0.5f && val <= 40.0f && sys_cfg.max_pressure != val) {
      sys_cfg.max_pressure = val;
      param_changed = 1;
    }
  }
  //电机最高转速
  if(start <= 102 && end >= 103) {
    uint32_t val = PACK32(usRegHoldingBuf[102], usRegHoldingBuf[103]);
    if(val >= 200 && val <= 3000 && sys_cfg.max_motor_speed != val) {
      sys_cfg.max_motor_speed = val;
      param_changed = 1;
    }
  }
  //电机最低转速
  if(start <= 104 && end >= 105) {
    uint32_t val = PACK32(usRegHoldingBuf[104], usRegHoldingBuf[105]);
    if(val >= 0 && val <= 500 && sys_cfg.min_motor_speed != val) {
      sys_cfg.min_motor_speed = val;
      param_changed = 1;
    }
  }
  //油温停机阈值
  if(start <= 106 && end >= 107) {
    uint32_t raw = PACK32(usRegHoldingBuf[106], usRegHoldingBuf[107]);
    float val = REG_TO_FLOAT((int32_t)raw);
    if(val >= 50.0f && val <= 120.0f && sys_cfg.max_oil_temp != val) {
      sys_cfg.max_oil_temp = val;
      param_changed = 1;
    }
  }
  //最低液位保护阈值
  if(start <= 108 && end >= 109) {
    uint32_t raw = PACK32(usRegHoldingBuf[108], usRegHoldingBuf[109]);
    float val = REG_TO_FLOAT((int32_t)raw);
    if(val >= 0.0f && val <= 50.0f && sys_cfg.min_liquid_level != val) {
      sys_cfg.min_liquid_level = val;
      param_changed = 1;
    }
  }
  //压力回差
  if(start <= 110 && end >= 111) {
    uint32_t raw = PACK32(usRegHoldingBuf[110], usRegHoldingBuf[111]);
    float val = REG_TO_FLOAT((int32_t)raw);
    if(val >= 0.1f && val <= 1.0f && sys_cfg.press_hysteresis != val) {
      sys_cfg.press_hysteresis = val;
      param_changed = 1;
    }
  }
  //超压安全裕量
  if(start <= 112 && end >= 113) {
    uint32_t raw = PACK32(usRegHoldingBuf[112], usRegHoldingBuf[113]);
    float val = REG_TO_FLOAT((int32_t)raw);
    if(val >= 0.0f && val <= 20.0f && sys_cfg.overpress_margin != val) {
      sys_cfg.overpress_margin = val;
      param_changed = 1;
    }
  }

  //远程设定基准目标压力 0114、0115
  if(start <= 114 && end >= 115)
  {
      uint32_t raw = PACK32(usRegHoldingBuf[114], usRegHoldingBuf[115]);
      float val = REG_TO_FLOAT((int32_t)raw);
      //压力范围 0.5 ~ 40.0MPa
      if(val >= 0.5f && val <= 40.0f && sys_cfg.ref_pressure != val)
      {
          sys_cfg.ref_pressure = val;
          param_changed = 1;
      }
  }

  //====================分区4 PID====================
  if(start <= 150 && end >= 151) {
    uint32_t raw = PACK32(usRegHoldingBuf[150], usRegHoldingBuf[151]);
    float f = REG_TO_FLOAT((int32_t)raw);
    if(f > 0 && sys_cfg.pid_kp != f) { sys_cfg.pid_kp = f; param_changed = 1; }
  }
  if(start <= 152 && end >= 153) {
    uint32_t raw = PACK32(usRegHoldingBuf[152], usRegHoldingBuf[153]);
    float f = REG_TO_FLOAT((int32_t)raw);
    if(f > 0 && sys_cfg.pid_ki != f) { sys_cfg.pid_ki = f; param_changed = 1; }
  }
  if(start <= 154 && end >= 155) {
    uint32_t raw = PACK32(usRegHoldingBuf[154], usRegHoldingBuf[155]);
    float f = REG_TO_FLOAT((int32_t)raw);
    if(f > 0 && sys_cfg.pid_kd != f) { sys_cfg.pid_kd = f; param_changed = 1; }
  }

  //====================分区5 通讯====================
  if(start == 170) {
    uint16_t addr = usRegHoldingBuf[170];
    if(addr >= 1 && addr <= 247 && sys_cfg.modbus_addr != addr) {
      sys_cfg.modbus_addr = addr;
      param_changed = 1;
    }
  }
  if(start == 171) {
    uint16_t sel = usRegHoldingBuf[171];
    if(sel >= 1 && sel <= 6 && sys_cfg.modbus_baud_sel != sel) {
      sys_cfg.modbus_baud_sel = sel;
      param_changed = 1;
    }
  }

  // RTC时间设置
  if(start <= regID_DateTime && regID_DateTime + 3 <= end) {
    uint64_t Utime = 0;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime] << 48;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 1] << 32;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 2] << 16;
    Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 3] << 0;
    if(Utime > 1735660800ULL && Utime < 2366812800ULL) {
      Unix_To_Time(Utime / 1000);
    }
  }

  // 参数发生变更，持久化Flash
  if(param_changed) {
    sync_to_sysdb();
  }
}

/**
 * @fn void deinit_sysdb()
 * @brief 反初始化数据库
 */
void deinit_sysdb() {
  fdb_kvdb_deinit(&kvdb);
}

/**
 * @brief 格式化数据库，恢复出厂默认sys_cfg
 */
int8_t flashdb_format_and_reset(void) {
  fdb_kvdb_deinit(&kvdb);
  init_sys_db();
  Cfg_SetDefault();
  sync_to_sysdb();
  LOG_Debug("FlashDB format and reset OK");
  return 0;
}

void SoftReset(void) {
  __set_FAULTMASK(1);
  NVIC_SystemReset();
}

void print_sysdb() {
  fdb_kv_print(&kvdb);
}

