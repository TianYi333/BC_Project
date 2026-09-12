#ifndef LOGIC_MAIN_H
#define LOGIC_MAIN_H

#include "main.h"
#include "project_config.h"
#include "buzzer.h"
#include "log.h"
#include "oled_task.h"
#include "rtc_clock.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include "pid_ctrl.h"
#include "modbus_master.h"


#define FAULT_DEBOUNCE_CNT    2U    // 连续2次判定有效故障

#define STOP_STABLE_DELAY_MS       1500U   // 停机后等待1.5秒，等待压力稳定

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

// =====================故障位图掩码【最终版 uint8_t】=====================
#define SENSOR_ERR_PRESSURE        (1U << 0)  // Bit0 压力传感器硬件故障
#define SENSOR_ERR_OIL_TEMP        (1U << 1)  // Bit1 油温传感器硬件故障
#define SENSOR_ERR_LIQUID          (1U << 2)  // Bit2 液位传感器485故障
#define SYS_ERR_MOTOR_DRIVER       (1U << 3)  // Bit3 电机驱动器本体故障(err_code/状态字故障位)
#define SYS_ERR_MOTOR_COMM_LOST    (1U << 4)  // Bit4 电机Modbus通讯丢失
#define SYS_ERR_SEVERE_OVERPRESS   (1U << 5)  // Bit5 严重超压（二级泄压故障）
#define SYS_ERR_OIL_TEMP_OVER      (1U << 6)  // Bit6 油液过温【锁定故障，需手动复位】
#define SYS_ERR_LOW_LIQUID         (1U << 7)  // Bit7 液位过低【锁定故障，需手动复位】

// 系统工作状态 sys_start
typedef enum
{
    SYS_STATE_STOP     = 0, // 停机：电机静止，无自动加压逻辑
    SYS_STATE_PRESS    = 1, // 加压：PID调速升压
    SYS_STATE_HOLD     = 2, // 保压：低速维持目标压力
    SYS_STATE_RELEASE  = 3, // 泄压：泄压阀打开释放压力
    SYS_STATE_SETTING  = 4, // 设置/调试模式：菜单编辑参数,上位机强制停机使用,禁止自动压力流程
    SYS_STATE_SELFTEST = 5, // 上电自检状态（新增）
    SYS_STATE_FAULT    = 6  // 故障锁定状态（新增）
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
    MOTOR_CTRL_MODE_E  set_ctrl_mode;  // 只写入6060 目标控制模式
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

	float             adc_pressure;	    //ch0 压力 MPa
	float             adc_oil_temp;	    //ch1 油液温度 ℃
	float             liquid_level_pct;	//油液位百分比
	uint8_t           valve_state;	    //阀芯状态，0=关闭，1=打开

	SENSOR_STATUS_E   sensor_status;    //传感器综合就绪状态
	uint8_t           sys_fault_bit;	//整机系统故障位图（汇总所有故障）
    uint8_t           fault_reset_req;  //故障手动复位请求 1=请求复位

    float             manual_set_speed; // 设置模式专用手动转速，仅SYS_STATE_SETTING下生效
} _SYS_STATUS;

/* =========泄压阀 GPIO 驱动接口=========
 * 阀门接 DO_OUTPUT_0 (PE3)，开阀=高电平(用户确认)。
 * 所有写 valve_state 的位置都应调用 Valve_Drive() 同步驱动实际引脚。
 * 注：PE3 原被 step_motor 用作电机 RDY 读取，已在 step_motor.c 屏蔽。
 */
void Valve_Drive(uint8_t open);

// 用户持久化配置参数，全部存入FlashDB
typedef struct
{
    // 压力控制参数
    float ref_pressure;          // 基准压力 MPa
    float max_pressure;          // 最高保护压力 MPa
    float press_hysteresis;      // 压力控制回差 MPa
    float overpress_margin;      // 超压安全裕量 MPa

    // 电机转速限制
    float max_motor_speed;       // 最大允许转速 RPM
    float min_motor_speed;       // 最小允许转速 RPM

    // 保护阈值
    float max_oil_temp;          // 最高油温保护 ℃
    float min_liquid_level;      // 最低液位保护 %

    // PID 参数
    float pid_kp;                // Proportional gain
    float pid_ki;                // Integral gain
    float pid_kd;                // Derivative gain

    // Modbus通讯配置
    uint8_t modbus_addr;         // 本机从站地址
    uint8_t modbus_baud_sel;    // 波特率选择码 1~6

    // 压力传感器4mA、20mA原始ADC校准值
    float press_cal_4ma_raw;
    float press_cal_20ma_raw;
    // 温度传感器4mA、20mA原始ADC校准值
    float temp_cal_4ma_raw;
    float temp_cal_20ma_raw;

} SYS_CONFIG_T;


// 故障上报结构体：所有子任务上报故障信号
typedef struct
{
    uint8_t flg_press_sensor_err;    //压力传感器故障
    uint8_t flg_temp_sensor_err;     //油温传感器故障
    uint8_t flg_liquid_sensor_err;   //液位传感器通讯故障
    //uint8_t flg_motor_comm_lost;     //电机通讯丢失
    //uint8_t flg_motor_driver_err;    //电机驱动器故障
} FAULT_REPORT_T;

// 报警记录最大存储条数
#define ALARM_RECORD_MAX_CNT 32U

// 单条报警记录结构体
typedef struct
{
    uint8_t fault_bit;       // 故障位图 local_fault_bit
    uint8_t hour;            // 故障发生时 时
    uint8_t min;             // 分
    uint8_t sec;             // 秒
    uint32_t tick;           // 系统tick（备用，精确时间戳）
} AlarmRecord;

// 全局报警存储
extern AlarmRecord g_alarm_records[ALARM_RECORD_MAX_CNT];
extern uint16_t g_alarm_record_cnt;

extern _SYS_STATUS main_sys_status;
extern SYS_CONFIG_T sys_cfg;
extern uint16_t motor_comm_err_cnt; // 电机Modbus连续通讯错误计数;
extern osMutexId_t sys_status_mutex;
extern FAULT_REPORT_T g_fault_report;

void SysStatus_Init(void);
int Cfg_SetDefault(void);

// 读取全局系统状态快照
// return: 1=成功拿到锁、拷贝有效；0=锁获取失败，降级裸拷贝（兜底）
uint8_t SysStatus_ReadSnapshot(_SYS_STATUS *dst);

// 修改全局系统状态
// return: 1=成功；0=锁获取失败，无法安全写入
uint8_t SysStatus_WriteSnapshot(_SYS_STATUS *src);

void MainLogicTask(void *argument);




#endif
