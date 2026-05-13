/*
 * syncif.c
 *
 *  Created on: Oct 16, 2025
 *      Author: 28038
 */

#include "syncif.h"
#include "flashdb.h"
#include "Msg_Queue.h"
#include "rtc_clock.h"
#include "semphr.h"
#include "fal.h"
#include "running_logic.h"
#include "mb.h"

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
	.priority =
		syncif_task_prio,
};

/**
 * @fn void start_syncif_task(void)
 * @brief 数据同步接口任务
 *
 */
void start_syncif_task()
{
	syncif_task_handler = osThreadNew(syncif_task, NULL,
									  &syncif_task_attributes);
}

void start_syncif_cycle_task()
{
	syncif_task_handler_cycle = osThreadNew(syncif_task_cycle, NULL,
											&syncif_task_attributes);
}

static void lock(fdb_db_t db)
{
	// taskENTER_CRITICAL(); /* 进入临界区 */
	// vTaskSuspendAll();
}

static void unlock(fdb_db_t db)
{
	// taskEXIT_CRITICAL(); /* 退出临界区 */
	// xTaskResumeAll();
}

uint16_t boot_count = 1;

uint32_t boot_time = 0;

/* default KV nodes */
static struct fdb_default_kv_node default_kv_table[] = {
	{"mfrs", "borsch", 0}, {"device_id", "1234567890kv", 0}, {"boot_count", &boot_count, sizeof(boot_count)}, {"boot_time", &boot_time, sizeof(boot_time)}, {"slaveID", &main_sys_status.slaveID, sizeof(main_sys_status.slaveID)}, {"injector_info", main_sys_status.injector, sizeof(main_sys_status.injector[0]) * injector_count}};
/* KVDB object */
static struct fdb_kvdb kvdb = {0};

/**
 * @fn void init_sys_db()
 * @brief 数据库的初始化函数
 *
 */
int8_t init_sys_db()
{
	fal_init();
	fdb_err_t result;
	struct fdb_default_kv default_kv;

	default_kv.kvs = default_kv_table;
	default_kv.num = sizeof(default_kv_table) / sizeof(default_kv_table[0]);
	kvdb.ver_num = sizeof(default_kv_table) / sizeof(default_kv_table[0]);
	/* set the lock and unlock function if you want */
	fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_LOCK, (void *)lock);
	fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_UNLOCK, (void *)unlock);
	result = fdb_kvdb_init(&kvdb, "sysdb", "ef_kvdb1", &default_kv, NULL);

	if (result != FDB_NO_ERR)
	{
		return -1;
	}

	return 1;
}

/**
 * @fn void syncif_task()
 * @brief 数据同步任务主函数
 *
 */
void syncif_task(void *argument)
{
    (void)argument;

	/**
	 * 初始化阶段要进行MB寄存器的初始化的数据归零
	 */

	vTaskDelay(2000);
	_MB_REG mb_Msg;
	init_sys_db();
	injector_timer_init();
	sync_from_sysdb();
	start_syncif_cycle_task();
	while (1)
	{
		if(xQueueReceive(QUEUE_CONN, &mb_Msg, 200) == pdPASS){
			taskENTER_CRITICAL();
			sync_from_modbus(mb_Msg);
			taskEXIT_CRITICAL();
		}
		else{
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
void syncif_task_cycle(void *argument)
{
    (void)argument;
	while (1)
	{
		vTaskDelay(300);

		sync_injector_timers();
	}
}
/**
 * @fn void sync_injector_timers()
 * @brief 同步定时器状态
 *
 */
void sync_injector_timers()
{
	uint8_t tag;
	if (main_sys_status.sys_start == 200)
	{
		for (tag = 0; tag < injector_count; tag++)
		{
			if (main_sys_status.injector[tag].interval > 0)
			{
				uint32_t time = main_sys_status.injector[tag].interval;
				if (xTimerGetPeriod(xTimeHandle[tag]) != time * 1000)
				{
					xTimerChangePeriod(xTimeHandle[tag], time * 1000, 1000);
				}
				if (xTimerIsTimerActive(xTimeHandle[tag]) == pdFALSE)
				{
					/* xTimer is active, do something. */
					xTimerStart(xTimeHandle[tag], 1000);
					logInfo("xTimerStart: %d ", tag);
				}
				TickType_t xRemainingTime = 0;
				xRemainingTime = xTimerGetExpiryTime(xTimeHandle[tag]);
				if (xRemainingTime != 0)
				{
					xRemainingTime -= xTaskGetTickCount();
				}
				main_sys_status.injector[tag].executionTime =
					(uint16_t)(xRemainingTime / 1000);
			}
		}
	}
	else
	{

		for (tag = 0; tag < injector_count; tag++)
		{
			xTimerStop(xTimeHandle[tag], 1000);
			if (main_sys_status.injector[tag].interval > 0)
			{
				uint32_t time = main_sys_status.injector[tag].interval;
				if (xTimerGetPeriod(xTimeHandle[tag]) != time * 1000)
				{
					xTimerChangePeriod(xTimeHandle[tag], time * 1000, 1000);
				}
				if (xTimerIsTimerActive(xTimeHandle[tag]) != pdFALSE)
				{
					/* xTimer is active, do something. */
					xTimerStop(xTimeHandle[tag], 1000);
				}
			}
		}
	}
}

/**
 * @fn void sync_from_sysdb()
 * @brief 从数据库读取数据
 *
 */
void sync_from_sysdb()
{
	struct fdb_blob blob;
	fdb_kv_get_blob(&kvdb, "slaveID",
					fdb_blob_make(&blob, &main_sys_status.slaveID,
								  sizeof(main_sys_status.slaveID)));
	fdb_kv_get_blob(&kvdb, "injector_info",
					fdb_blob_make(&blob, main_sys_status.injector,
								  sizeof(main_sys_status.injector[0]) * injector_count));
	uint8_t tag = 0;
	for (tag = 0;
		 tag < (sizeof(main_sys_status.injector) / sizeof(main_sys_status.injector[0])); tag++)
	{
		main_sys_status.injector[tag].executionTime = 0;
		main_sys_status.injector[tag].executionVol = 0;
		main_sys_status.injector[tag].injectRequest = 0;
		main_sys_status.injector[tag].tagTime = 0;
	}
	main_sys_status.is_Busy = 1;
	main_sys_status.p_model.point = motor_loc_list[0];
	main_sys_status.q_model.point = motor_loc_list[0];
}

/**
 * @fn void sync_to_sysdb()
 * @brief 同步数据到数据库
 *
 */
void sync_to_sysdb()
{
	struct fdb_blob blob;
	fdb_kv_set_blob(&kvdb, "slaveID",
					fdb_blob_make(&blob, &main_sys_status.slaveID,
								  sizeof(main_sys_status.slaveID)));
	fdb_kv_set_blob(&kvdb, "injector_info",
					fdb_blob_make(&blob, main_sys_status.injector,
								  sizeof(main_sys_status.injector[0]) * injector_count));
}
/**
 * @fn void sync_to_modbus()
 * @brief 同步数据到modbus寄存器数组
 *
 */
void sync_to_modbus()
{

	usRegHoldingBuf[regID_IsRunning] = main_sys_status.sys_start;
	uint64_t Utime = Time_To_Unix() * 1000;
	usRegHoldingBuf[regID_DateTime] = Utime >> 48;
	usRegHoldingBuf[regID_DateTime + 1] = Utime >> 32;
	usRegHoldingBuf[regID_DateTime + 2] = Utime >> 16;
	usRegHoldingBuf[regID_DateTime + 3] = Utime >> 0;

	usRegHoldingBuf[regID_SlaveID] = main_sys_status.slaveID;
	uint8_t tag = 0;
	for (tag = 0;
		 tag < (sizeof(main_sys_status.injector) / sizeof(main_sys_status.injector[0])); tag++)
	{
		usRegHoldingBuf[300 + (tag * 10) + 0] =
			main_sys_status.injector[tag].interval;
		usRegHoldingBuf[300 + (tag * 10) + 1] =
			main_sys_status.injector[tag].volume;
		usRegHoldingBuf[300 + (tag * 10) + 2] =
			main_sys_status.injector[tag].injectRequest;
		usRegHoldingBuf[300 + (tag * 10) + 3] =
			main_sys_status.injector[tag].executionVol;
		usRegHoldingBuf[300 + (tag * 10) + 4] =
			main_sys_status.injector[tag].executionTime;
		usRegHoldingBuf[300 + (tag * 10) + 5] =
		    main_sys_status.injector[tag].injectRequest;

		usRegHoldingBuf[300 + (tag * 10) + 6] =
			main_sys_status.injector[tag].tagTime >> 48;
		usRegHoldingBuf[300 + (tag * 10) + 7] =
			main_sys_status.injector[tag].tagTime >> 32;
		usRegHoldingBuf[300 + (tag * 10) + 8] =
			main_sys_status.injector[tag].tagTime >> 16;
		usRegHoldingBuf[300 + (tag * 10) + 9] =
			main_sys_status.injector[tag].tagTime >> 0;
	}

}
/**
 * @fn void sync_from_modbus()
 * @brief 从modbus寄存器数组同步数据
 *
 */
void sync_from_modbus(_MB_REG mbMsg)
{
	
	if (regID_IsRunning >= mbMsg.startAddr && regID_IsRunning <= (mbMsg.startAddr + mbMsg.len))
	{
		main_sys_status.sys_start = usRegHoldingBuf[regID_IsRunning];
	}

	// 判断时间寄存器是否在修改范围内
	if (regID_DateTime >= mbMsg.startAddr && regID_DateTime <= (mbMsg.startAddr + mbMsg.len))
	{

		uint64_t Utime = 0;

		Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime] << 48;
		Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 1] << 32;
		Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 2] << 16;
		Utime |= (uint64_t)usRegHoldingBuf[regID_DateTime + 3] << 0;

		if ((Utime > 1735660800000) && (Utime < 2366812800000))
		{
			Unix_To_Time(Utime / 1000);
		}
	}
	if (regID_SlaveID >= mbMsg.startAddr && regID_SlaveID <= (mbMsg.startAddr + mbMsg.len))
	{
		if (main_sys_status.slaveID != usRegHoldingBuf[regID_SlaveID])
		{
			main_sys_status.slaveID = usRegHoldingBuf[regID_SlaveID];
			struct fdb_blob blob;
			fdb_kv_set_blob(&kvdb, "slaveID",
							fdb_blob_make(&blob, &main_sys_status.slaveID,
										  sizeof(main_sys_status.slaveID)));
		}
	}
	if (regID_Injector <= mbMsg.startAddr && regID_Injector <= (mbMsg.startAddr + mbMsg.len))
	{
		uint8_t kv_tag = 0;
		uint8_t tag = 0;
		for (tag = 0;
			 tag < (sizeof(main_sys_status.injector) / sizeof(main_sys_status.injector[0])); tag++)
		{
			if (main_sys_status.injector[tag].interval != usRegHoldingBuf[300 + (tag * 10) + 0])
			{
				main_sys_status.injector[tag].interval = usRegHoldingBuf[300 + (tag * 10) + 0];
				kv_tag++;
			}
			if (main_sys_status.injector[tag].volume != usRegHoldingBuf[300 + (tag * 10) + 1])
			{
				main_sys_status.injector[tag].volume = usRegHoldingBuf[300 + (tag * 10) + 1];
				kv_tag++;
			}
			main_sys_status.injector[tag].injectRequest = usRegHoldingBuf[300 + (tag * 10) + 2];
			// 如果系统状态不是预备状态就不接受任务
			if (main_sys_status.running_mode == SYS_MODE_READY)
			{
				if (usRegHoldingBuf[300 + (tag * 10) + 3] > 0 && main_sys_status.is_Busy == 0)
				{
					task_object_list[0].task_status = 1;
					task_object_list[0].inject_id = tag + 1;
					task_object_list[0].val = usRegHoldingBuf[300 + (tag * 10) + 3];
				}
			}
		}
		if (kv_tag > 0)
		{
			struct fdb_blob blob;
			fdb_kv_set_blob(&kvdb, "injector_info",
							fdb_blob_make(&blob, main_sys_status.injector,
										  sizeof(main_sys_status.injector[0]) * injector_count));
		}
	}
}

/**
 * @fn void deinit_sysdb()
 * @brief 反初始化系统数据库
 *
 */
void deinit_sysdb()
{
	fdb_kvdb_deinit(&kvdb);
}

SHELL_EXPORT_CMD(
	SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
	resetdb, deinit_sysdb, "This is a command that reset system database.");

void SoftReset(void)
{
	__set_FAULTMASK(1); // 关闭所有中断
	NVIC_SystemReset(); // 请求系统复位
}

SHELL_EXPORT_CMD(
	SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
	reboot, SoftReset, "This is a command that reboot system.");

void print_sysdb()
{
	fdb_kv_print(&kvdb);
}

SHELL_EXPORT_CMD(
	SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
	printdb, print_sysdb, "This is a command that print database.");
