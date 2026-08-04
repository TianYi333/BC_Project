#include "modbus_master.h"


uint8_t mb_tx_buf[MB_TX_BUF_LEN] = {0};
uint8_t mb_rx_buf[MB_RX_BUF_LEN] = {0};
uint16_t motor_comm_err_cnt = 0U;// 电机Modbus连续通讯错误计数
uint16_t liquid_comm_err_cnt = 0U;// 液位传感器Modbus连续通讯错误计数
osSemaphoreId_t mb_semaphore;// 总线互斥信号量
osSemaphoreId_t mb_rx_semaphore;// 接收完成信号量（DMA+空闲中断唤醒）
static uint16_t mb_rx_frame_len = 0;// 本次收到帧长度

void MB_Semaphore_Init(void);
void ModbusMasterTask(void *argument);
uint16_t mb_crc16(uint8_t *data, uint16_t len);
void rs485_tx_en(void);
void rs485_rx_en(void);
int mb_master_read_holding(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_out);
int mb_master_write_single(uint8_t slave_addr, uint16_t reg_addr, uint16_t val);
int mb_master_write_multi(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_in);
uint16_t Motor_ReadErrCode(void);
uint16_t Motor_ReadStatusWord(void);
uint16_t Motor_ReadRunMode(void);
int Motor_SetRunMode(uint16_t mode_val);
int Motor_SetTargetSpeed(float speed_rpm);
float Motor_ReadRealSpeed(void);
int Motor_SetAcc(uint16_t acc_val);
uint16_t Motor_ReadAcc(void);
int Motor_SetDec(uint16_t dec_val);
uint16_t Motor_ReadDec(void);
uint16_t LiquidSensor_ReadLevel(void); // 读取液位传感器液位值
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size);


void MB_Semaphore_Init(void)
{
    mb_semaphore = osSemaphoreNew(1, 1, NULL);
    mb_rx_semaphore = osSemaphoreNew(1, 0, NULL);
    // 启动DMA空闲接收
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
}

void ModbusMasterTask(void *argument)
{
    MB_Semaphore_Init();
    MOTOR_STATUS_T motor_tmp;
    static uint8_t poll_step = 0;
    static uint8_t liquid_tick_cnt = 0; // 液位采集计数器
    uint16_t liquid_raw = 0;
    for(;;)
    {
        //分组轮换读取，减少单次报文数量
        switch(poll_step)
        {
            case 0:
                motor_tmp.err_code       = Motor_ReadErrCode();
                motor_tmp.status_word    = Motor_ReadStatusWord();
                break;
            case 1:
                motor_tmp.real_ctrl_mode = Motor_ReadRunMode();
                motor_tmp.actual_speed   = (uint32_t)Motor_ReadRealSpeed();
                break;
            case 2:
                motor_tmp.target_acc     = Motor_ReadAcc();
                motor_tmp.target_dec     = Motor_ReadDec();
                break;
        }
        poll_step = (poll_step + 1) % 3;

        // 每200ms计数+1，5次 = 1000ms读取一次液位
        liquid_tick_cnt++;
        if(liquid_tick_cnt >= 5U)
        {
            liquid_raw = LiquidSensor_ReadLevel();
            liquid_tick_cnt = 0U;
        }

        //临界区更新全局电机状态
        taskENTER_CRITICAL();
        // 只刷新读取到的只读字段，不覆盖上层设置字段(set_ctrl_mode/target_speed)
        main_sys_status_1.motor_status.err_code       = motor_tmp.err_code;
        main_sys_status_1.motor_status.status_word    = motor_tmp.status_word;
        main_sys_status_1.motor_status.real_ctrl_mode = motor_tmp.real_ctrl_mode;
        main_sys_status_1.motor_status.actual_speed   = motor_tmp.actual_speed;
        main_sys_status_1.motor_status.target_acc     = motor_tmp.target_acc;
        main_sys_status_1.motor_status.target_dec     = motor_tmp.target_dec;
        // ============判断通讯丢失标志 comm_lost============
        if(motor_comm_err_cnt >= MOTOR_COMM_ERR_MAX)
        {
            main_sys_status_1.motor_status.comm_lost = 1U;
        }
        else
        {
            main_sys_status_1.motor_status.comm_lost = 0U;
        }

        if(liquid_tick_cnt == 0U)
        {
            // 示例公式：假设传感器0~4000原始值对应0~100%，按你传感器手册修改！
            main_sys_status_1.liquid_level_pct = (float)liquid_raw / 4000.0f * 100.0f;
        }
        taskEXIT_CRITICAL();

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// Modbus CRC16查表
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
    uint16_t crc = 0xFFFF;
    uint8_t idx;
    while(len--)
    {
        idx = crc ^ *data++;
        crc >>= 8;
        crc ^= crc16_table[idx];
    }
    return crc;
}

// 485切换发送 PG3拉高
void rs485_tx_en(void)
{
    HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_DIR_PIN, GPIO_PIN_SET);
}
// 485切换接收 PG3拉低
void rs485_rx_en(void)
{
    HAL_GPIO_WritePin(RS485_DIR_PORT, RS485_DIR_PIN, GPIO_PIN_RESET);
}

/**
 * @brief 非阻塞DMA版 读保持寄存器
 */
int mb_master_read_holding(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_out)
{
    if(osSemaphoreAcquire(mb_semaphore, pdMS_TO_TICKS(200)) != osOK)
        return -4; // 获取总线信号量失败

    uint8_t tx_len = 0;
    uint16_t crc, recv_crc, calc_crc;
    memset(mb_tx_buf, 0, MB_TX_BUF_LEN);

    // 组装请求帧
    mb_tx_buf[tx_len++] = slave_addr;
    mb_tx_buf[tx_len++] = MB_FUNC_READ_HOLDING;
    mb_tx_buf[tx_len++] = (reg_start >> 8) & 0xFF;
    mb_tx_buf[tx_len++] = reg_start & 0xFF;
    mb_tx_buf[tx_len++] = (reg_num >> 8) & 0xFF;
    mb_tx_buf[tx_len++] = reg_num & 0xFF;
    crc = mb_crc16(mb_tx_buf, tx_len);
    mb_tx_buf[tx_len++] = crc & 0xFF;
    mb_tx_buf[tx_len++] = (crc >> 8) & 0xFF;

    // 清除上一轮残留接收信号
    osSemaphoreAcquire(mb_rx_semaphore, 0);
    mb_rx_frame_len = 0;

    rs485_tx_en();
    // DMA启动发送，函数立即返回，无忙等
    HAL_UART_Transmit_DMA(&huart3, mb_tx_buf, tx_len);

    // RTOS阻塞等待应答，任务主动让出CPU
    if(osSemaphoreAcquire(mb_rx_semaphore, MB_RX_TIMEOUT) != osOK)
    {
        HAL_UART_DMAStop(&huart3);
        rs485_rx_en();
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
        osSemaphoreRelease(mb_semaphore);
        return -1; // 应答超时
    }

    // 计算有效长度：从机地址+功能码+数据长度+N字节数据+2CRC
    uint8_t data_len = mb_rx_buf[2];
    uint16_t rx_len = 3 + data_len + 2;

    if(rx_len < 5 || mb_rx_frame_len < rx_len)
    {
        osSemaphoreRelease(mb_semaphore);
        return -3; // 帧长度非法
    }
    // 判断异常帧
    if((mb_rx_buf[1] & 0x80) != 0)
    {
        osSemaphoreRelease(mb_semaphore);
        return -5; // Modbus异常应答
    }

    // CRC校验
    recv_crc = mb_rx_buf[rx_len - 2] | ((uint16_t)mb_rx_buf[rx_len - 1] << 8);
    calc_crc = mb_crc16(mb_rx_buf, rx_len - 2);
    if(recv_crc != calc_crc)
    {
        osSemaphoreRelease(mb_semaphore);
        return -2; // CRC错误
    }

    // 解析寄存器数据
    for(uint8_t i = 0; i < data_len / 2; i++)
    {
        data_out[i] = ((uint16_t)mb_rx_buf[3 + i*2] << 8) | mb_rx_buf[4 + i*2];
    }

    osSemaphoreRelease(mb_semaphore);
    return 0;
}

/**
 * @brief 非阻塞DMA版 单寄存器写入
 */
int mb_master_write_single(uint8_t slave_addr, uint16_t reg_addr, uint16_t val)
{
    if(osSemaphoreAcquire(mb_semaphore, pdMS_TO_TICKS(200)) != osOK)
    {
        return -4;
    }

    uint8_t tx_len = 0;
    uint16_t crc, recv_crc, calc_crc;
    memset(mb_tx_buf, 0, MB_TX_BUF_LEN);

    mb_tx_buf[tx_len++] = slave_addr;
    mb_tx_buf[tx_len++] = MB_FUNC_WRITE_SINGLE;
    mb_tx_buf[tx_len++] = (reg_addr >> 8) & 0xFF;
    mb_tx_buf[tx_len++] = reg_addr & 0xFF;
    mb_tx_buf[tx_len++] = (val >> 8) & 0xFF;
    mb_tx_buf[tx_len++] = val & 0xFF;
    crc = mb_crc16(mb_tx_buf, tx_len);
    mb_tx_buf[tx_len++] = crc & 0xFF;
    mb_tx_buf[tx_len++] = (crc >> 8) & 0xFF;

    osSemaphoreAcquire(mb_rx_semaphore, 0);
    mb_rx_frame_len = 0;

    rs485_tx_en();
    HAL_UART_Transmit_DMA(&huart3, mb_tx_buf, tx_len);

    if(osSemaphoreAcquire(mb_rx_semaphore, MB_RX_TIMEOUT) != osOK)
    {
        HAL_UART_DMAStop(&huart3);
        rs485_rx_en();
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
        osSemaphoreRelease(mb_semaphore);
        return -1;
    }

    // 单写应答固定8字节
    uint16_t rx_len = 8;
    if(mb_rx_frame_len != rx_len)
    {
        osSemaphoreRelease(mb_semaphore);
        return -3;
    }
    if(mb_rx_buf[1] & 0x80)
    {
        osSemaphoreRelease(mb_semaphore);
        return -5;
    }

    recv_crc = mb_rx_buf[6] | ((uint16_t)mb_rx_buf[7] << 8);
    calc_crc = mb_crc16(mb_rx_buf, 6);
    if(recv_crc != calc_crc)
    {
        osSemaphoreRelease(mb_semaphore);
        return -2;
    }

    osSemaphoreRelease(mb_semaphore);
    return 0;
}

/**
 * @brief 非阻塞DMA版 批量寄存器写入
 */
int mb_master_write_multi(uint8_t slave_addr, uint16_t reg_start, uint16_t reg_num, uint16_t *data_in)
{
    if(osSemaphoreAcquire(mb_semaphore, pdMS_TO_TICKS(200)) != osOK)
    {
        return -4;
    }

    uint8_t tx_len = 0;
    uint16_t crc, recv_crc, calc_crc;
    uint8_t data_byte = reg_num * 2;
    memset(mb_tx_buf, 0, MB_TX_BUF_LEN);

    mb_tx_buf[tx_len++] = slave_addr;
    mb_tx_buf[tx_len++] = MB_FUNC_WRITE_MULTI;
    mb_tx_buf[tx_len++] = (reg_start >> 8) & 0xFF;
    mb_tx_buf[tx_len++] = reg_start & 0xFF;
    mb_tx_buf[tx_len++] = (reg_num >> 8) & 0xFF;
    mb_tx_buf[tx_len++] = reg_num & 0xFF;
    mb_tx_buf[tx_len++] = data_byte;

    for(uint8_t i = 0; i < reg_num; i++)
    {
        mb_tx_buf[tx_len++] = (data_in[i] >> 8) & 0xFF;
        mb_tx_buf[tx_len++] = data_in[i] & 0xFF;
    }
    crc = mb_crc16(mb_tx_buf, tx_len);
    mb_tx_buf[tx_len++] = crc & 0xFF;
    mb_tx_buf[tx_len++] = (crc >> 8) & 0xFF;

    osSemaphoreAcquire(mb_rx_semaphore, 0);
    mb_rx_frame_len = 0;

    rs485_tx_en();
    HAL_UART_Transmit_DMA(&huart3, mb_tx_buf, tx_len);

    if(osSemaphoreAcquire(mb_rx_semaphore, MB_RX_TIMEOUT) != osOK)
    {
        HAL_UART_DMAStop(&huart3);
        rs485_rx_en();
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
        osSemaphoreRelease(mb_semaphore);
        return -1;
    }

    uint16_t rx_len = 8;
    if(mb_rx_frame_len != rx_len)
    {
        osSemaphoreRelease(mb_semaphore);
        return -3;
    }
    if(mb_rx_buf[1] & 0x80)
    {
        osSemaphoreRelease(mb_semaphore);
        return -5;
    }

    recv_crc = mb_rx_buf[6] | ((uint16_t)mb_rx_buf[7] << 8);
    calc_crc = mb_crc16(mb_rx_buf, 6);
    if(recv_crc != calc_crc)
    {
        osSemaphoreRelease(mb_semaphore);
        return -2;
    }

    osSemaphoreRelease(mb_semaphore);
    return 0;
}


// 读取电机故障码 1001 返回0：正常；非0：故障码
uint16_t Motor_ReadErrCode(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 1001, 1, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read motor err code fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

//读取电机状态字 6041
uint16_t Motor_ReadStatusWord(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 6041, 1, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read status word fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

//读取当前运行模式 6061
uint16_t Motor_ReadRunMode(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 6061, 1, reg_data);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read runmode fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return reg_data[0];
}

//写运行模式 6060（06 单寄存器写）
int Motor_SetRunMode(uint16_t mode_val)
{
    int ret = mb_master_write_single(MOTOR_SLAVE_ADDR, 0x6060, mode_val);
    if(ret != 0)
    {
        motor_comm_err_cnt ++;
        LOG_mb("SetRunMode comm fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0;
    }
    return ret;
}

//下发目标转速 6081（32 位，16 功能码批量写 2 个寄存器）
int Motor_SetTargetSpeed(float speed_rpm)
{
    if(speed_rpm > main_sys_status_1.max_motor_speed) speed_rpm = main_sys_status_1.max_motor_speed;
    if(speed_rpm < -main_sys_status_1.max_motor_speed) speed_rpm = -main_sys_status_1.max_motor_speed;
    // RPM → 驱动器0x6081寄存器值
    int32_t reg_val = (int32_t)(speed_rpm / 6.0f);

    uint16_t reg_buf[2];
    reg_buf[0] = (reg_val >> 16) & 0xFFFFU;
    reg_buf[1] = reg_val & 0xFFFFU;

    int ret = mb_master_write_multi(MOTOR_SLAVE_ADDR, 0x6081, 2, reg_buf);
    if(ret != 0)
    {
        motor_comm_err_cnt ++;
        LOG_mb("SetSpeed comm fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0;
    }
    return ret;
}

// 读取实际反馈转速 606C（32 位，读 2 寄存器）
float Motor_ReadRealSpeed(void)
{
    uint16_t reg_buf[2] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 0x606C, 2, reg_buf);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("Read real speed fail,ret=%d",ret);
        return 0.0f;
    }
    motor_comm_err_cnt = 0U;

    // 拼接有符号32位原始寄存器值
    int32_t raw_reg = ((int32_t)reg_buf[0] << 16U) | reg_buf[1];
    // 原始寄存器 → RPM
    float rpm = (float)raw_reg * 6.0f;
    return rpm;
}

// 读写加减速参数 6083/6084
int Motor_SetAcc(uint16_t acc_val)
{
    int ret = mb_master_write_single(MOTOR_SLAVE_ADDR, 6083, acc_val);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("SetAcc fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return ret;
}

uint16_t Motor_ReadAcc(void)
{
    uint16_t dat[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 6083, 1, dat);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("ReadAcc fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return dat[0];
}

int Motor_SetDec(uint16_t dec_val)
{
    int ret = mb_master_write_single(MOTOR_SLAVE_ADDR, 6084, dec_val);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("SetDec fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return ret;
}

uint16_t Motor_ReadDec(void)
{
    uint16_t dat[1] = {0};
    int ret = mb_master_read_holding(MOTOR_SLAVE_ADDR, 6084, 1, dat);
    if(ret != 0)
    {
        motor_comm_err_cnt++;
        LOG_mb("ReadDec fail,ret=%d",ret);
    }
    else
    {
        motor_comm_err_cnt = 0U;
    }
    return dat[0];
}

// 读取液位传感器液位值
uint16_t LiquidSensor_ReadLevel(void)
{
    uint16_t reg_data[1] = {0};
    int ret = mb_master_read_holding(LIQUID_SLAVE_ADDR, LIQUID_REG_LEVEL, 1, reg_data);
    if(ret != 0)
    {
        liquid_comm_err_cnt++;
        LOG_mb("Read liquid sensor fail,ret=%d",ret);
        // 液位通讯故障，置位传感器故障bit
        taskENTER_CRITICAL();
        main_sys_status_1.sensor_err_bit |= SENSOR_ERR_LIQUID;
        taskEXIT_CRITICAL();
        return 0U;
    }
    else
    {
        liquid_comm_err_cnt = 0U;
        // 清除液位故障标志
        taskENTER_CRITICAL();
        main_sys_status_1.sensor_err_bit &= ~SENSOR_ERR_LIQUID;
        taskEXIT_CRITICAL();
    }
    return reg_data[0];
}

/**
 * @brief DMA发送完成回调，切换485为接收
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart->Instance == USART3)
    {
        rs485_rx_en();
    }
}

/**
 * @brief 空闲中断DMA接收回调（HAL_UARTEx_ReceiveToIdle_DMA）
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if(huart->Instance == USART3)
    {
        mb_rx_frame_len = Size;
        osSemaphoreRelease(mb_rx_semaphore);
        // 重新启动DMA空闲接收，持续等待下一帧
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, mb_rx_buf, MB_RX_BUF_LEN);
    }
}
