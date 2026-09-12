#include "main_logic.h"
#include "net_comm_task.h"   // tcp_send_motor_event_report（电机启停事件主动上报）
#include "ota.h"             // ota_app_confirm（OTA 启动确认 / 防砖确认点）

void MainLogicTask(void *argument)
{
    for(;;)
    {
        osDelay(pdMS_TO_TICKS(200));
        ota_app_confirm();

    }
}



