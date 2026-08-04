#ifndef LOGIC_MAIN_H
#define LOGIC_MAIN_H

#include "main.h"
#include "buzzer.h"
#include "log.h"
#include "oled_task.h"
#include "rtc_clock.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include "pid_ctrl.h"
#include "modbus_master.h"


#define LOG_Debug(fmt, ...) printf("[%s:%d] " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)

#define MOTOR_STATUS_FAULT_BIT     (1U << 3)// 驱动器0x6041状态字：Bit3 = 驱动器错误标志
#define STOP_STABLE_DELAY_MS    3000U   // 停机后等待3秒，等待压力稳定

// 0x1001 错误寄存器 位掩码定义
#define MOT_ERR_BIT_NORMAL        (1U << 0)  // 常规错误
#define MOT_ERR_BIT_CURRENT       (1U << 1)  // 电流错误
#define MOT_ERR_BIT_VOLTAGE       (1U << 2)  // 电压错误
#define MOT_ERR_BIT_TEMP          (1U << 3)  // 温度报警
#define MOT_ERR_BIT_COMM          (1U << 4)  // 通信错误
#define MOT_ERR_BIT_PHASE_LOSS    (1U << 5)  // 电机缺相
#define MOT_ERR_BIT_POS_DEV       (1U << 6)  // 位置超差

// 0x6041 状态字 Bit掩码
#define STATUS_BIT_READY_INIT      (1U << 0)   // Bit0：1=准备好初始化
#define STATUS_BIT_INIT_FINISH     (1U << 1)   // Bit1：初始化完成
#define STATUS_BIT_ENABLE          (1U << 2)   // Bit2：电机上电使能
#define STATUS_BIT_FAULT           (1U << 3)   // Bit3：1=驱动器故障
#define STATUS_BIT_WORKING         (1U << 4)   // Bit4：1=驱动器正常上电工作
#define STATUS_BIT_QUICK_STOP      (1U << 5)   // Bit5：1=快速停止
#define STATUS_BIT_INIT_MODE       (1U << 6)   // Bit6：1=进入初始化状态
#define STATUS_BIT_WARN            (1U << 7)   // Bit7：1=警告
#define STATUS_BIT_PAUSE           (1U << 8)   // Bit8：1=电机暂停运行
#define STATUS_BIT_RUNNING         (1U << 9)   // Bit9：1=电机运行标志
#define STATUS_BIT_IN_POSITION     (1U << 10)  // Bit10：到位标志
#define STATUS_BIT_HOME_SW         (1U << 11)  // Bit11：机械原点限位标志
#define STATUS_BIT_SPEED_ZERO      (1U << 12)  // Bit12：速度等于0
#define STATUS_BIT_POS_OVERRUN     (1U << 13)  // Bit13：电机位置超差
#define STATUS_BIT_LIMIT_CW        (1U << 14)  // Bit14：CW正向限位
#define STATUS_BIT_LIMIT_CCW       (1U << 15)  // Bit15：CCW反向限位

// 传感器故障位图掩码
#define SENSOR_ERR_PRESSURE    (1U << 0)  // 压力传感器故障
#define SENSOR_ERR_OIL_TEMP    (1U << 1)  // 油温传感器故障
#define SENSOR_ERR_LIQUID      (1U << 2)  // 液位传感器(485)故障

// 系统工作状态 sys_start
typedef enum
{
    SYS_STATE_STOP     = 0, // 停机：电机静止，无自动加压逻辑
    SYS_STATE_PRESS    = 1, // 加压：PID调速升压
    SYS_STATE_HOLD     = 2, // 保压：低速维持目标压力
    SYS_STATE_RELEASE  = 3, // 泄压：泄压阀打开释放压力
    SYS_STATE_SETTING  = 4, // 参数设置：菜单编辑参数，禁止自动压力流程
    SYS_STATE_SELFTEST = 5  // 上电自检状态（新增）
} SYS_WORK_STATE_E;

// 传感器综合状态
typedef enum
{
	SENSOR_STATUS_FAULT = 0,
	SENSOR_STATUS_READY = 1
}SENSOR_STATUS_E;


// 电机状态枚举
typedef enum
{
    MOTOR_STATE_STOP     = 0U, // 停止
    MOTOR_STATE_RUN      = 1U, // 运行
    MOTOR_STATE_WAIT     = 2U, // 等待
    MOTOR_STATE_FAULT    = 3U  // 故障锁定
} MOTOR_STATE_E;

// 操作模式枚举
typedef enum
{
    MOTOR_CTRL_MODE_POS     = 1,    // 位置模式
    MOTOR_CTRL_MODE_VEL     = 3,    // 速度模式
    MOTOR_CTRL_MODE_TORQUE  = 4,    // 转矩模式（步进不支持）
    MOTOR_CTRL_MODE_HOMING  = 6     // 回零模式
} MOTOR_CTRL_MODE_E;


/**
 * @brief 电机状态结构体
 * 寄存器映射关系：
 * err_code      -> 1001(03读)
 * status_word   -> 6041(03读)
 * real_ctrl_mode-> 6061(03读)
 * set_ctrl_mode -> 6060(06写)
 * target_speed  -> 6081(32bit,03读/10写)
 * target_acc    -> 6083(16bit,03读/06写)
 * target_dec    -> 6084(16bit,03读/06写)
 * actual_speed  -> 606C(32bit,03读)
 */
// 电机状态结构体
typedef struct
{
    // 软件解析状态（上层逻辑使用，无对应寄存器）
    MOTOR_STATE_E      state;          // 电机软件运行状态
    
    // =========电机Modbus通讯丢失标志 1=通讯丢失，0=通讯正常=========
    uint8_t            comm_lost;
    
    // Modbus原始读取数据（只读寄存器）
    uint16_t           err_code;       // 寄存器1001 故障码 0=无故障
    uint16_t           status_word;    // 寄存器6041 驱动器原始状态字
    MOTOR_CTRL_MODE_E  real_ctrl_mode; // 寄存器6061 当前实际运行模式
    
    // Modbus下发设置参数（读写寄存器）
    MOTOR_CTRL_MODE_E  set_ctrl_mode;  // 待写入6060 目标控制模式
    uint32_t           target_speed;   // 寄存器6081 32bit 目标转速 RPM
    uint16_t           target_acc;     // 寄存器6083 16bit 加速度 rpm/s
    uint16_t           target_dec;     // 寄存器6084 16bit 减速度 rpm/s

    // Modbus实时反馈值（只读）
    uint32_t           actual_speed;   // 寄存器606C 32bit 实际转速 RPM
} MOTOR_STATUS_T;


typedef struct{
	SYS_WORK_STATE_E  sys_start;	    //整机工作状态
	MOTOR_STATUS_T    motor_status;	    //电机完整状态
	uint8_t           sys_warning;      //系统警告状态，0=无警告，1=有警告

	float adc_pressure;			    //ch0 压力 MPa
	float adc_oil_temp;			    //ch1 油液温度 ℃
	float liquid_level_pct;		    //油液位百分比
	uint8_t valve_state;		    //阀芯状态，0=关闭，1=打开
	float ref_pressure;			    //目标参考压力 MPa

	float max_pressure;                 //最高保护压力 MPa
	float max_motor_speed;              //电机允许最大转速 RPM
	float min_motor_speed;              //电机允许最低转速 RPM
	float max_oil_temp;                 //最高油温保护 ℃
	float min_liquid_level;             //最低液位保护 %

	SENSOR_STATUS_E sensor_status;      //传感器综合就绪状态
	uint8_t         sensor_err_bit;     //传感器故障位图

    float pid_kp;
	float pid_ki;
	float pid_kd;
    
} _SYS_STATUS_1;

extern _SYS_STATUS_1 main_sys_status_1;
extern uint16_t motor_comm_err_cnt; // 电机Modbus连续通讯错误计数;
extern const float PRESS_HYSTERESIS;

void MainLogicTask(void *argument);




#endif
