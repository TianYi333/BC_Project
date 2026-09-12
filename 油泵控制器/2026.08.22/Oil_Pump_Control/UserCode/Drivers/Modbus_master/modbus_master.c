#include "modbus_master.h"

uint8_t mb_tx_buf[MB_TX_BUF_LEN] = {0};
uint8_t mb_rx_buf[MB_RX_BUF_LEN] = {0};
uint16_t motor_comm_err_cnt = 0U;    // 电机Modbus连续通讯错误计数
uint16_t liquid_comm_err_cnt = 0U;   // 液位传感器Modbus连续通讯错误计数

extern SYS_CONFIG_T sys_cfg;

//====总线互斥锁（替换原二元信号量mb_semaphore）====
osMutexId_t mb_bus_mutex;
// 接收事件二元信号量（中断可用，不变）
osSemaphoreId_t mb_rx_semaphore;
// FreeRTOS原生消息队列句柄
QueueHandle_t mb_msg_queue = NULL;

static uint16_t mb_rx_frame_len = 0; // 本次收到帧长度

/* -------- Modbus 请求回执静态池 开始 -------- */
#define MB_ACK_POOL_CNT     4U          //最大并发请求数
typedef struct
{
    osSemaphoreId_t     sem;
    uint8_t             used;
} MB_ACK_POOL_T;

static MB_ACK_POOL_T  g_mb_ack_pool[MB_ACK_POOL_CNT];


//====================电机写函数改为static，仅本文件内部可见====================、
#if defined(MOTOR_DRIVER_RS485_SERVO)
static int Motor_SetRunMode(uint16_t mode_val);
static int Motor_SetTargetSpeed(float speed_rpm);
static int Motor_SetAcc(uint16_t acc_val);
static int Motor_SetDec(uint16_t dec_val);
#endif

/**
 * @brief  获取空闲回执单元
 * @retval NULL:无空闲
 */
static MB_ACK_POOL_T * MB_GetFreeAckPool(void)
{
    for(uint8_t i = 0; i < MB_ACK_POOL_CNT; i++)
    {
        if(g_mb_ack_pool[i].used == 0U)
        {
            g_mb_ack_pool[i].used = 1U;
            return &g_mb_ack_pool[i];
        }
    }
    return NULL;
}

/**
 * @brief  归还回执单元，清除信号残留
 */
static void MB_ReturnAckPool(MB_ACK_POOL_T *pItem)
{
    if(pItem == NULL)
        return;
    osSemaphoreAcquire(pItem->sem, 0U);
    pItem->used = 0U;
}
/* -------- Modbus 请求回执静态池 结束 -------- */

// CRC16查表
static const uint16_t crc16_table[256] = {
0x0000,0xC0C1,0xC181,0x0140,0xC301,0x03C0,0x0280,0xC241,0xC601,0x06C0,0x0780,0xC741,0x0500,0xC5C1,0xC481,0x0440,
0xCC01,0x0CC0,0x0D80,0xCD41,0x0F00,0xCFC1,0xCE81,0x0E40,0x0A00,0xCAC1,0xCB81,0x0B40,0xC901,0x09C0,0x0880,0xC841,
0xD801,0x18C0,0x1980,0xD941,0x1B00,0xDBC1,0xDA81,0x1A40,0x1E00,0xDEC1,0xDF81,0x1F40,0xDD01,0x1DC0,0x1C80,0xDC41,
0x1400,0xD4C1,0xD581,0x1540,0xD701,0x17C0,0x1680,0xD641,0xD201,0x12C0,0x1380,0xD341,0x1100,0xD1C1,0xD081,0x1040,
0xF001,0x30C0,0x3180,0xF141,0x3300,0xF3C1,0xF281,0x3240,0x3600,0xF6C1,0xF781,0x3740,0xF501,0x35C0,0x3480,0xF441,
0x3C00,0xFCC1,0xFD81,0x3D40,0xFF01,0x3FC0,0x3E80,0xFE41,0xFA01,0x3AC0,0x3B80,0xFB41,0x3900,0xF9C1,0xF881,0x3840,
0x2800,0xE8C1,0xE981,0x2940,0xEB01,0x2BC0,0x2A80,0xEA41,0xEE01,0x2EC0,0x2F80,0xEF41,0x2D00,0xEDC1,0xEC81,0x2C40,
0xE401,0x24C0,0x2580,0xE541,0x2700,0xE7C1,0xE681,0x2640,0x2200,0xE2C1,0xE381,0x2340,0xE101,0x21C0,0x2080,0xE041,
0xA001,0x60C0,0x6180,0xA141,0x6300,0xA3C1,0xA281,0x6240,0x6600,0xA6C1,0xA781,0x6740,0xA501,0x65C0,0x6480,0xA441,
0x6C00,0xACC1,0xAD81,0x6D40,0xAF01,0x6FC0,0x6E80,0xAE41,0xAA01,0x6AC0,0x6B80,0xAB41,0x6900,0xA9C1,0xA881,0x6840,
0x7800,0xB8C1,0xB981,0x7940,0xBB01,0x7BC0,0x7A80,0xBA41,0xBE01,0x7EC0,0x7F80,0xBF41,0x7D00,0xBDC1,0xBC81,0x7C40,
0xB401,0x74C0,0x7580,0xB541,0x7700,0xB7C1,0xB681,0x7640,0x7200,0xB2C1,0xB381,0x7340,0xB101,0x71C0,0x7080,0xB041,
0x5000,0x90C1,0x9181,0x5140,0x9301,0x53C0,0x5280,0x9241,0x9601,0x56C0,0x5780,0x9741,0x5500,0x95C1,0x9481,0x5440,
0x9C01,0x5CC0,0x5D80,0x9D41,0x5F00,0x9FC1,0x9E81,0x5E40,0x5A00,0x9AC1,0x9B81,0x5B40,0x9901,0x59C0,0x5880,0x9841,
0x8801,0x48C0,0x4980,0x8941,0x4B00,0x8BC1,0x8A81,0x4A40,0x4E00,0x8EC1,0x8F81,0x4F40,0x8D01,0x4DC0,0x4C80,0x8C41,
0x4400,0x84C1,0x8581,0x4540,0x8701,0x47C0,0x4680,0x8641,0x8201,0x42C0,0x4380,0x8341,0x4100,0x81C1,0x8081,0x4040,
};

uint16_t mb_crc16(uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint8_t  idx;
    while(len--)
    {
        idx = (uint8_t)(crc ^ *data++);
        crc = (crc >> 8U) ^ crc16_table[idx];
    }
    return crc;
}

void rs485_tx_en(void)
{
    HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_DIR_PIN, GPIO_PIN_SET);
}

void rs485_rx_en(void)
{
    HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_DIR_PIN, GPIO_PIN_RESET);
}

// 初始化互斥锁、接收信号量、FreeRTOS消息队列、DMA接收
void MB_Semaphore_Init(void)
{
    mb_bus_mutex = osMutexNew(NULL);
    mb_rx_semaphore = osSemaphoreNew(1U, 0U, NULL);
    mb_msg_queue = xQueueCreate(MB_MSG_QUEUE_DEPTH, sizeof(MB_MSG_T));

    for(uint8_t i = 0; i < MB_ACK_POOL_CNT; i++)
    {
        g_mb_ack_pool[i].sem = osSemaphoreNew(1U, 0U, NULL);
        g_mb_ack_pool[i].used = 0U;
    }

    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
}

/**
 * @brief 外部任务统一提交Modbus读写请求，同步阻塞等待完成
 * @param req_msg 消息结构体指针
 * @param wait_ms 总超时ms
 * @return 0成功，负数错误码
 */
int MB_SubmitRequest(MB_MSG_T *req_msg, uint32_t wait_ms)
{
    MB_ACK_POOL_T *ack_item = MB_GetFreeAckPool();
    if(ack_item == NULL)
    {
        return -8;      //并发已满
    }
    req_msg->ack_sem = ack_item->sem;
    req_msg->ret_code = -99;

    BaseType_t send_ret = xQueueSend(mb_msg_queue, req_msg, pdMS_TO_TICKS(wait_ms));
    if(send_ret != pdPASS)
    {
        MB_ReturnAckPool(ack_item);
        return -7;      //队列满
    }

    if(osSemaphoreAcquire(ack_item->sem, pdMS_TO_TICKS(wait_ms)) != osOK)
    {
        MB_ReturnAckPool(ack_item);
        return -1;      //超时
    }

    int ret = req_msg->ret_code;
    MB_ReturnAckPool(ack_item);
    return ret;
}

//====================对外统一提交接口，业务层全部调用这一组====================
int MotorCtrl_SubmitSetMode(uint16_t mode_val)//设置运行模式
{
    MB_MSG_T msg = {0};
    msg.op = MB_OP_WRITE_SINGLE;
    msg.slave_addr = MOTOR_SLAVE_ADDR;
    msg.reg_start  = 0x6060U;
    msg.val_single = mode_val;

    int ret = MB_SubmitRequest(&msg, 200U);
    return ret;
}

int MotorCtrl_SubmitSetSpeed(float speed_rpm)//设置速度
{
    MB_MSG_T msg = {0};
    float max_spd = sys_cfg.max_motor_speed;

    if(speed_rpm > max_spd)
        speed_rpm = max_spd;
    if(speed_rpm < 0.0f)
        speed_rpm = 0.0f;

    int32_t reg_val = (int32_t)(speed_rpm / 6.0f);

    msg.op = MB_OP_WRITE_MULTI;
    msg.slave_addr = MOTOR_SLAVE_ADDR;
    msg.reg_start  = 0x6081U;
    msg.reg_num    = 2U;

    msg.data_buf[0] = (uint16_t)((reg_val >> 16) & 0xFFFFU);
    msg.data_buf[1] = (uint16_t)(reg_val & 0xFFFFU);
    msg.p_data = msg.data_buf;

    int ret = MB_SubmitRequest(&msg, 200U);
    return ret;
}

int MotorCtrl_SubmitSetAcc(uint16_t acc_val)//设置加速度
{
    MB_MSG_T msg = {0};
    msg.op = MB_OP_WRITE_SINGLE;
    msg.slave_addr = MOTOR_SLAVE_ADDR;
    msg.reg_start  = 0x6083U;
    msg.val_single = acc_val;

    int ret = MB_SubmitRequest(&msg, 200U);
    return ret;
}

int MotorCtrl_SubmitSetDec(uint16_t dec_val)//设置减速速度
{
    MB_MSG_T msg = {0};
    msg.op = MB_OP_WRITE_SINGLE;
    msg.slave_addr = MOTOR_SLAVE_ADDR;
    msg.reg_start  = 0x6084U;
    msg.val_single = dec_val;

    int ret = MB_SubmitRequest(&msg, 200U);
    return ret;
}



#if defined(MOTOR_DRIVER_RS485_SERVO)
// 伺服电机版本 ModbusMasterTask：伺服轮询 + 液位轮询 + mb_msg_queue
// Modbus主机任务：优先处理外部请求，后台轮询电机/液位
void ModbusMasterTask(void *argument)
{
    MB_Semaphore_Init();
    MOTOR_STATUS_T motor_tmp = {0};
    static uint8_t poll_step = 0;
    static uint8_t liquid_tick_cnt = 0;
    static uint8_t acc_dec_need_refresh = 1U; // 加减速刷新标记：上电读一次；外部修改参数后置位重新读取
    uint16_t liquid_raw = 0;
    uint8_t liquid_read_ok = 1;
    MB_MSG_T msg;


    //======== 上电：伺服速度模式强制校准阶段 ========  
    uint16_t read_real_mode = 0U;
    static uint8_t init_liquid_tick = 0U;   //初始化阶段液位分频计数器
    uint16_t liquid_raw_init = 0U;
    uint8_t liquid_read_ok_init = 1U;

    LOG_mb("Motor mode init, waiting velocity mode...");
    while(1)
    {
        read_real_mode = Motor_ReadRunMode();
        _SYS_STATUS snap_tmp;
        if(SysStatus_ReadSnapshot(&snap_tmp))
        {
            if(motor_comm_err_cnt == 0U)   //读取通讯正常
            {
                uint8_t need_save = 0U;
                if(snap_tmp.motor_status.real_ctrl_mode != read_real_mode)// 读到真实模式，更新
                {
                    snap_tmp.motor_status.real_ctrl_mode = read_real_mode;
                    need_save = 1U;
                }
                if(snap_tmp.motor_status.comm_lost != 0U)// 之前标记为通讯丢失，现已恢复
                {
                    snap_tmp.motor_status.comm_lost = 0U;
                    need_save = 1U;
                }
                if(need_save != 0U)
                {
                    SysStatus_WriteSnapshot(&snap_tmp);
                }


                if(read_real_mode == MOTOR_CTRL_MODE_VEL)
                {
                    LOG_mb("Motor already in velocity mode, init ok");
                    break;
                }
                //模式不对，设置速度模式
                int ret = Motor_SetRunMode(MOTOR_CTRL_MODE_VEL);
                if(ret == 0)
                {
                    LOG_mb("Set motor to velocity mode success");
                }
                else
                {
                    LOG_mb("Set motor velocity mode write fail");
                }
            }
            else
            {
                LOG_mb("Read motor mode comm error during init");
                if(motor_comm_err_cnt >= MOTOR_COMM_ERR_MAX)//通讯错误次数超过最大值
                {
                    if(snap_tmp.motor_status.comm_lost != 1U)
                    {
                        snap_tmp.motor_status.comm_lost = 1U;
                        SysStatus_WriteSnapshot(&snap_tmp);
                    }
                }
            }
        }

        //==================== 初始化阶段液位读取逻辑 ====================
        init_liquid_tick++;
        if(init_liquid_tick >= 5U) // 5*200ms = 1000ms读一次液位
        {
            init_liquid_tick = 0U;
            uint16_t reg_buf[1] = {0};
            int liq_ret = mb_master_read_holding(LIQUID_SLAVE_ADDR, LIQUID_REG_LEVEL, 1U, reg_buf);
            if(liq_ret != 0)
            {
                if(liquid_comm_err_cnt < 3U)
                {
                    liquid_comm_err_cnt++;
                }
                LOG_mb("Init stage read liquid sensor fail,ret=%d cnt=%d", liq_ret, liquid_comm_err_cnt);
                liquid_read_ok_init = 0U;
                liquid_raw_init = 0U;
            }
            else
            {
                liquid_read_ok_init = 1U;
                liquid_comm_err_cnt = 0U;
                liquid_raw_init = reg_buf[0];
            }

            // 更新液位传感器故障标记
            g_fault_report.flg_liquid_sensor_err = (liquid_comm_err_cnt >= 3U) ? 1U : 0U;

            // 读到有效数据，更新快照液位百分比
            _SYS_STATUS liq_snap;
            if(SysStatus_ReadSnapshot(&liq_snap))
            {
                if(liquid_read_ok_init)
                {
                    liq_snap.liquid_level_pct = (float)liquid_raw_init / 4000.0f * 100.0f;
                }
                SysStatus_WriteSnapshot(&liq_snap);
            }
        }
        //================================================================

        osDelay(pdMS_TO_TICKS(200U));
    }
    //======== 模式校准完成，进入正常业务循环 ========
    for(;;)
    {
        //第一优先级：处理外部提交的总线请求，非阻塞取出
        if(xQueueReceive(mb_msg_queue, &msg, pdMS_TO_TICKS(100U)) == pdPASS)
        {
            int ret = 0;
            switch(msg.op)
            {
                case MB_OP_READ_HOLD:
                    ret = mb_master_read_holding(msg.slave_addr, msg.reg_start, msg.reg_num, msg.p_data);
                    break;
                case MB_OP_WRITE_SINGLE:
                    ret = mb_master_write_single(msg.slave_addr, msg.reg_start, msg.val_single);
                    // 写单寄存器：判断是否是加速度、减速度寄存器
                    if(ret == 0 && msg.slave_addr == MOTOR_SLAVE_ADDR)
                    {
                        // 替换为你实际驱动器加速度、减速度寄存器号
                        if((msg.reg_start == 0x6083U) || (msg.reg_start == 0x6084U))
                        {
                            acc_dec_need_refresh = 1U;
                        }
                    }
                    break;
                case MB_OP_WRITE_MULTI:
                    ret = mb_master_write_multi(msg.slave_addr, msg.reg_start, msg.reg_num, msg.p_data);
                    if(ret == 0 && msg.slave_addr == MOTOR_SLAVE_ADDR)
                    {
                        uint16_t reg_end = msg.reg_start + msg.reg_num - 1U;
                        //判断批量写入区间是否包含0x6083或0x6084
                        if( (0x6083U >= msg.reg_start && 0x6083U <= reg_end) ||
                            (0x6084U >= msg.reg_start && 0x6084U <= reg_end) )
                        {
                            acc_dec_need_refresh = 1U;
                        }
                    }
                    break;
                default:
                    ret = -6;
                    break;
            }
            msg.ret_code = ret;
            osSemaphoreRelease(msg.ack_sem);
            continue;
        }


        //第二优先级：后台轮询逻辑
        uint8_t run_liquid = 0U;
        liquid_tick_cnt++;
        if(liquid_tick_cnt >= 5U)
        {
            run_liquid = 1U;
            liquid_tick_cnt = 0U;
        }


        if(run_liquid == 0U)
        {
            switch(poll_step)
            {
                case 0://错误码
                    motor_tmp.err_code       = Motor_ReadErrCode();//读取错误码
                    break;

                case 1://状态字
                    motor_tmp.status_word    = Motor_ReadStatusWord();//读取状态字
                    break;

                case 2://实际控制模式
                    motor_tmp.real_ctrl_mode = Motor_ReadRunMode();//读取实际控制模式
                    //读到最新模式立刻判断，通讯正常且非速度模式，下发设置
                    if(motor_comm_err_cnt == 0U && motor_tmp.real_ctrl_mode != MOTOR_CTRL_MODE_VEL)
                    {
                        int ret_set = Motor_SetRunMode(MOTOR_CTRL_MODE_VEL);//设置速度模式
                        if(ret_set == 0)
                        {
                            LOG_mb("Poll detect not vel mode, send set vel cmd");
                        }
                        else
                        {
                            LOG_mb("Poll set vel mode fail ret=%d", ret_set);
                        }
                    }
                    break;

                case 3://实际速度，单位：rpm
                    motor_tmp.actual_speed   = (uint32_t)Motor_ReadRealSpeed();//读取实际速度
                    break;
            }
            poll_step = (poll_step + 1U) % 4U;
        }
        else
        {
            uint16_t reg_data[1] = {0};
            int ret = mb_master_read_holding(LIQUID_SLAVE_ADDR, LIQUID_REG_LEVEL, 1U, reg_data);
            if(ret != 0)
            {
                if(liquid_comm_err_cnt < 3U)
                {
                    liquid_comm_err_cnt++;
                }
                LOG_mb("Read liquid sensor fail,ret=%d cnt=%d", ret, liquid_comm_err_cnt);
                liquid_read_ok = 0U;
                liquid_raw = 0U;
            }
            else
            {
                liquid_read_ok = 1U;
                liquid_comm_err_cnt = 0U;
                liquid_raw = reg_data[0];
            }
        }


        // 上电 / 参数修改标记置位时，读取一次加减速，读完清除标记
        if(acc_dec_need_refresh)
        {
            motor_tmp.target_acc = Motor_ReadAcc();
            motor_tmp.target_dec = Motor_ReadDec();
            acc_dec_need_refresh = 0U;
        }


        // 连续3次通讯失败才判定液位传感器故障
        g_fault_report.flg_liquid_sensor_err = (liquid_comm_err_cnt >= 3U) ? 1U : 0U;


        //快照更新系统状态
        _SYS_STATUS write_tmp;
        if(SysStatus_ReadSnapshot(&write_tmp))
        {
            write_tmp.motor_status.err_code       = motor_tmp.err_code;
            write_tmp.motor_status.status_word    = motor_tmp.status_word;
            write_tmp.motor_status.real_ctrl_mode = motor_tmp.real_ctrl_mode;
            write_tmp.motor_status.actual_speed   = motor_tmp.actual_speed;
            write_tmp.motor_status.target_acc     = motor_tmp.target_acc;
            write_tmp.motor_status.target_dec     = motor_tmp.target_dec;
            write_tmp.motor_status.comm_lost      = (motor_comm_err_cnt >= MOTOR_COMM_ERR_MAX) ? 1U : 0U;


            if(run_liquid && liquid_read_ok)
            {
                write_tmp.liquid_level_pct = (float)liquid_raw / 4000.0f * 100.0f;
            }
            SysStatus_WriteSnapshot(&write_tmp);
        }


        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}


#elif defined(MOTOR_DRIVER_PULSE_STEPPER)
// 脉冲步进电机版本 ModbusMasterTask：仅液位轮询 + mb_msg_queue，无伺服电机任何逻辑
void ModbusMasterTask(void *argument)
{
    mb_bus_mutex = osMutexNew(NULL);
    mb_rx_semaphore = osSemaphoreNew(1U, 0U, NULL);
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);

    uint16_t liquid_raw = 0;//液位原始值
    uint8_t liquid_read_ok = 1;//液位读取是否成功

    for(;;)
    {
        uint16_t reg_data[1] = {0};
        int ret = mb_master_read_holding(LIQUID_SLAVE_ADDR, LIQUID_REG_LEVEL, 1U, reg_data);
        if(ret != 0)
        {
            if(liquid_comm_err_cnt < 3U)
            {
                liquid_comm_err_cnt++;
            }
            LOG_mb("Read liquid sensor fail,ret=%d cnt=%d", ret, liquid_comm_err_cnt);
            liquid_read_ok = 0U;
            liquid_raw = 0U;
        }
        else
        {
            liquid_read_ok = 1U;
            liquid_comm_err_cnt = 0U;
            liquid_raw = reg_data[0];
        }

        // 连续3次通讯失败才判定液位传感器故障
        g_fault_report.flg_liquid_sensor_err = (liquid_comm_err_cnt >= 3U) ? 1U : 0U;

        _SYS_STATUS write_tmp;
        if(SysStatus_ReadSnapshot(&write_tmp))
        {
            if(liquid_read_ok)
            {
                write_tmp.liquid_level_pct = (float)liquid_raw / 4000.0f * 100.0f;
            }
            SysStatus_WriteSnapshot(&write_tmp);
        }

        vTaskDelay(pdMS_TO_TICKS(500U));
    }
}
#endif


//读保持寄存器
int mb_master_read_holding(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_out)
{
    if(osMutexAcquire(mb_bus_mutex, pdMS_TO_TICKS(200U)) != osOK)
        return -4;

    uint8_t tx_len = 0U;
    uint16_t crc, recv_crc, calc_crc;
    memset(mb_tx_buf, 0, MB_TX_BUF_LEN);

    mb_tx_buf[tx_len++] = slave_addr;
    mb_tx_buf[tx_len++] = MB_FUNC_READ_HOLDING;
    mb_tx_buf[tx_len++] = (uint8_t)((reg_start >> 8) & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)(reg_start & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)((reg_num >> 8) & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)(reg_num & 0xFFU);

    crc = mb_crc16(mb_tx_buf, tx_len);
    mb_tx_buf[tx_len++] = (uint8_t)(crc & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)((crc >> 8) & 0xFFU);

    osSemaphoreAcquire(mb_rx_semaphore, 0U);
    mb_rx_frame_len = 0U;

    rs485_tx_en();
    HAL_UART_Transmit_DMA(&huart3, mb_tx_buf, tx_len);

    if(osSemaphoreAcquire(mb_rx_semaphore, MB_RX_TIMEOUT) != osOK)
    {
        HAL_UART_DMAStop(&huart3);
        rs485_rx_en();
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
        osMutexRelease(mb_bus_mutex);
        return -1;
    }

    uint8_t data_len = mb_rx_buf[2];
    uint16_t rx_total_len = 3U + data_len + 2U;
    if(rx_total_len < 5U || mb_rx_frame_len < rx_total_len)
    {
        osMutexRelease(mb_bus_mutex);
        return -3;
    }
    if((mb_rx_buf[1] & 0x80U) != 0U)
    {
        osMutexRelease(mb_bus_mutex);
        return -5;
    }

    recv_crc  = (uint16_t)mb_rx_buf[rx_total_len-2] | ((uint16_t)mb_rx_buf[rx_total_len-1] << 8U);
    calc_crc = mb_crc16(mb_rx_buf, rx_total_len - 2U);
    if(recv_crc != calc_crc)
    {
        osMutexRelease(mb_bus_mutex);
        return -2;
    }

    for(uint8_t i = 0; i < (data_len/2U); i++)
    {
        data_out[i] = ((uint16_t)mb_rx_buf[3 + i*2] << 8U) | mb_rx_buf[4 + i*2];
    }

    osMutexRelease(mb_bus_mutex);
    return 0;
}

//单寄存器写 0x06
int mb_master_write_single(uint8_t slave_addr, uint16_t reg_addr, uint16_t val)
{
    if(osMutexAcquire(mb_bus_mutex, pdMS_TO_TICKS(200U)) != osOK)
        return -4;

    uint8_t tx_len = 0U;
    uint16_t crc, recv_crc, calc_crc;
    memset(mb_tx_buf, 0, MB_TX_BUF_LEN);

    mb_tx_buf[tx_len++] = slave_addr;
    mb_tx_buf[tx_len++] = MB_FUNC_WRITE_SINGLE;
    mb_tx_buf[tx_len++] = (uint8_t)((reg_addr >> 8) & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)(reg_addr & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)((val >> 8) & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)(val & 0xFFU);

    crc = mb_crc16(mb_tx_buf, tx_len);
    mb_tx_buf[tx_len++] = (uint8_t)(crc & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)((crc >> 8) & 0xFFU);

    osSemaphoreAcquire(mb_rx_semaphore, 0U);
    mb_rx_frame_len = 0U;

    rs485_tx_en();
    HAL_UART_Transmit_DMA(&huart3, mb_tx_buf, tx_len);

    if(osSemaphoreAcquire(mb_rx_semaphore, MB_RX_TIMEOUT) != osOK)
    {
        HAL_UART_DMAStop(&huart3);
        rs485_rx_en();
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
        osMutexRelease(mb_bus_mutex);
        return -1;
    }

    if(mb_rx_frame_len != 8U)
    {
        osMutexRelease(mb_bus_mutex);
        return -3;
    }
    if((mb_rx_buf[1] & 0x80U) != 0U)
    {
        osMutexRelease(mb_bus_mutex);
        return -5;
    }

    recv_crc  = (uint16_t)mb_rx_buf[6] | ((uint16_t)mb_rx_buf[7] << 8U);
    calc_crc = mb_crc16(mb_rx_buf, 6U);
    if(recv_crc != calc_crc)
    {
        osMutexRelease(mb_bus_mutex);
        return -2;
    }

    osMutexRelease(mb_bus_mutex);
    return 0;
}

//多寄存器写 0x10
int mb_master_write_multi(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_in)
{
    if(osMutexAcquire(mb_bus_mutex, pdMS_TO_TICKS(200U)) != osOK)
        return -4;

    uint8_t tx_len = 0U;
    uint16_t crc, recv_crc, calc_crc;
    uint8_t byte_cnt = (uint8_t)(reg_num * 2U);
    memset(mb_tx_buf, 0, MB_TX_BUF_LEN);

    mb_tx_buf[tx_len++] = slave_addr;
    mb_tx_buf[tx_len++] = MB_FUNC_WRITE_MULTI;
    mb_tx_buf[tx_len++] = (uint8_t)((reg_start >> 8) & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)(reg_start & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)((reg_num >> 8) & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)(reg_num & 0xFFU);
    mb_tx_buf[tx_len++] = byte_cnt;

    for(uint16_t i = 0; i < reg_num; i++)
    {
        mb_tx_buf[tx_len++] = (uint8_t)((data_in[i] >> 8) & 0xFFU);
        mb_tx_buf[tx_len++] = (uint8_t)(data_in[i] & 0xFFU);
    }

    crc = mb_crc16(mb_tx_buf, tx_len);
    mb_tx_buf[tx_len++] = (uint8_t)(crc & 0xFFU);
    mb_tx_buf[tx_len++] = (uint8_t)((crc >> 8) & 0xFFU);

    osSemaphoreAcquire(mb_rx_semaphore, 0U);
    mb_rx_frame_len = 0U;

    rs485_tx_en();
    HAL_UART_Transmit_DMA(&huart3, mb_tx_buf, tx_len);

    if(osSemaphoreAcquire(mb_rx_semaphore, MB_RX_TIMEOUT) != osOK)
    {
        HAL_UART_DMAStop(&huart3);
        rs485_rx_en();
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
        osMutexRelease(mb_bus_mutex);
        return -1;
    }

    if(mb_rx_frame_len != 8U)
    {
        osMutexRelease(mb_bus_mutex);
        return -3;
    }
    if((mb_rx_buf[1] & 0x80U) != 0U)
    {
        osMutexRelease(mb_bus_mutex);
        return -5;
    }

    recv_crc  = (uint16_t)mb_rx_buf[6] | ((uint16_t)mb_rx_buf[7] << 8U);
    calc_crc = mb_crc16(mb_rx_buf, 6U);
    if(recv_crc != calc_crc)
    {
        osMutexRelease(mb_bus_mutex);
        return -2;
    }

    osMutexRelease(mb_bus_mutex);
    return 0;
}

//电机轮询读接口，仅ModbusMasterTask内部循环调用
uint16_t Motor_ReadErrCode(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x1001U, 1U, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor err code fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

uint16_t Motor_ReadStatusWord(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x6041U, 1U, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor status word fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

uint16_t Motor_ReadRunMode(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x6061U, 1U, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor run mode fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

float Motor_ReadRealSpeed(void)
{
    uint16_t reg_data[2] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x606CU, 2U, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor real speed fail ret=%d", ret);
        return 0.0f;
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    int32_t raw = ((int32_t)reg_data[0] << 16U) | reg_data[1];
    return (float)raw * 6.0f;
}

uint16_t Motor_ReadAcc(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x6083U, 1U, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor acc fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

uint16_t Motor_ReadDec(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x6084U, 1U, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor dec fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

#if defined(MOTOR_DRIVER_RS485_SERVO)
// static电机写实现，仅本文件内部（保留，可用于调试）
static int Motor_SetRunMode(uint16_t mode_val)
{
    int ret = mb_master_write_single(MOTOR_SLAVE_ADDR, 0x6060U, mode_val);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Motor set run mode fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return ret;
}

static int Motor_SetTargetSpeed(float speed_rpm)
{
    float max_spd = sys_cfg.max_motor_speed;
    if(speed_rpm > max_spd) speed_rpm = max_spd;
    if(speed_rpm < 0.0f) speed_rpm = 0.0f;

    int32_t reg_val = (int32_t)(speed_rpm / 6.0f);
    uint16_t buf[2];
    buf[0] = (uint16_t)((reg_val >> 16) & 0xFFFFU);
    buf[1] = (uint16_t)(reg_val & 0xFFFFU);

    int ret = mb_master_write_multi(MOTOR_SLAVE_ADDR, 0x6081U, 2U, buf);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Motor set speed fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return ret;
}

static int Motor_SetAcc(uint16_t acc_val)
{
    int ret = mb_master_write_single(MOTOR_SLAVE_ADDR, 0x6083U, acc_val);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Motor set acc fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return ret;
}

static int Motor_SetDec(uint16_t dec_val)
{
    int ret = mb_master_write_single(MOTOR_SLAVE_ADDR, 0x6084U, dec_val);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Motor set dec fail ret=%d", ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return ret;
}
#endif

uint16_t LiquidSensor_ReadLevel(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(LIQUID_SLAVE_ADDR, LIQUID_REG_LEVEL, 1U, reg_data);
    if(ret != 0)
    {
        LOG_mb("Liquid read level fail ret=%d", ret);
        return 0U;
    }
    return reg_data[0];
}

//DMA发送完成回调
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart->Instance == USART3)
    {
        rs485_rx_en();
    }
}

//空闲中断接收回调
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if(huart->Instance == USART3)
    {
        mb_rx_frame_len = Size;
        osSemaphoreRelease(mb_rx_semaphore);
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
    }
}


