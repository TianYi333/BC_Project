#include "syncif.h"
#include "cmsis_os.h"
#include "main.h"
#include "cmsis_os.h"
#include "fal.h"
#include "flashdb.h"
#include "rtc_clock.h"
#include "semphr.h"
#include "task.h"
#include "project_config.h"
#include <string.h>

extern SemaphoreHandle_t flash_kv_mutex;

/* KVDB 仅持久化网络配置 net_cfg，不再维护 sys 默认键值表 */

/* KVDB对象：内部Flash上仅持久化网络配置 net_cfg，供各模块直接访问。 */
struct fdb_kvdb kvdb = {0};
static bool g_kvdb_inited = false;

/**
 * FlashDB互斥锁回调
 */
void lock(fdb_db_t db) {
  (void)db;
  if (flash_kv_mutex == NULL)
  {
      return;
  }
  xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
}

void unlock(fdb_db_t db) {
  (void)db;
  if (flash_kv_mutex == NULL)
  {
      return;
  }
  xSemaphoreGiveRecursive(flash_kv_mutex);
}

/**
 * @fn int8_t init_sys_db()
 * @brief 数据库初始化
 */
int8_t init_sys_db() {
  fdb_err_t result;

  /* 单库只初始化一次，避免 net/reboot 多处调用重复 Init/格式化 */
  if (g_kvdb_inited) {
    return 1;
  }

  kvdb.ver_num = APP_DB_VERSION;
  fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_LOCK, (void *)lock);
  fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_UNLOCK, (void *)unlock);
  result = fdb_kvdb_init(&kvdb, "sysdb", "ef_kvdb1", NULL, NULL);
  if (result != FDB_NO_ERR) {
    return -1;
  }
  g_kvdb_inited = true;
  return 1;
}


/**
 * @fn void deinit_sysdb()
 * @brief 反初始化数据库
 */
void deinit_sysdb() {
  fdb_kvdb_deinit(&kvdb);
  g_kvdb_inited = false;
}

/**
 * @brief 格式化数据库，恢复出厂默认sys_cfg
 */
int8_t flashdb_format_and_reset(void) {
  deinit_sysdb();
  init_sys_db();
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

