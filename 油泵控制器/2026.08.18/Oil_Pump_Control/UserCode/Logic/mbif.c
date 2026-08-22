/*
 * mbif.c
 *
 *  Created on: Oct 15, 2025
 *      Author: 28038
 */
#include <stdint.h>
#include "mbif.h"
#include "mbReg.h"
#include "stm32h7xx_hal_tim.h"
#include "syncif.h"
#include "main_logic.h"


volatile uint8_t slave_addr = 0x01;


// 根据配置码转实际波特率数值
uint32_t GetModbusBaudBySel(uint8_t sel)
{
    switch(sel)
    {
        case 1: return 2400;
        case 2: return 4800;
        case 3: return 9600;
        case 4: return 19200;
        case 5: return 38400;
		case 6: return 115200;
        default: return 115200; // 默认115200
    }
}

/**
 * modbus的任务句柄
 */
TaskHandle_t MBif_task_handler;

/**
 * modbus任务参数
 */
const osThreadAttr_t MBif_task_attributes = {
    .name = "MBifTask",
    .stack_size = mbif_task_stk_size,
    .priority = mbif_task_prio,
};

/**
 * @fn void start_mbif_task(void)
 * @brief modbus RTU接口任务创建
 */
void start_mbif_task() {
	MBif_task_handler = osThreadNew(MBif_task, NULL, &MBif_task_attributes);
}

/**
 * @fn void MBif_task()
 * @brief modbus RTU任务主循环
 */
void MBif_task() {
	vTaskDelay(pdMS_TO_TICKS(10000));

    // 从统一sys_cfg读取从站地址、波特率选择码
    slave_addr = sys_cfg.modbus_addr;
    uint32_t baudrate = GetModbusBaudBySel(sys_cfg.modbus_baud_sel);

	// 初始化Modbus RTU
	eMBInit(MB_RTU, slave_addr, 1, baudrate, MB_PAR_NONE);//FreeModbus 初始化串口、帧接收、底层移植接口（RTU 模式）；
	eMBEnable();//FreeModbus 启用Modbus RTU接口；

	while (1) {
	    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if(GetSerialRestartRequestAndClear() == pdTRUE)
        {
            // 先关闭modbus，重置串口底层
            eMBDisable();
            xMBPortSerialRestart();
            eMBEnable();
            continue;
        }
        eMBPoll();
	}
}
