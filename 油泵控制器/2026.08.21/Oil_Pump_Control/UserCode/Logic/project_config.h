#ifndef __PROJECT_CONFIG_H
#define __PROJECT_CONFIG_H


// 电机驱动二选一
//#define MOTOR_DRIVER_RS485_SERVO
#define MOTOR_DRIVER_PULSE_STEPPER
// 编译断言
#if defined(MOTOR_DRIVER_RS485_SERVO) && defined(MOTOR_DRIVER_PULSE_STEPPER)
#error "cannot define two motor driver!"
#endif
//================================================================================================

// ================================ 打印配置 ================================
//#define LOG_NET(fmt, ...) ((void)0)
#define LOG_NET(fmt, ...) printf("[NET] " fmt "\r\n", ##__VA_ARGS__)

#define LOG_mb(fmt, ...) (void)0
//#define LOG_mb(fmt, ...) printf("[mb] " fmt "\r\n", ##__VA_ARGS__)

//#define LOG_ADC(fmt, ...) (void)0
#define LOG_ADC(fmt, ...) printf("[ADC] " fmt "\r\n", ##__VA_ARGS__)

//#define LOG_logic(fmt, ...) (void)0
#define LOG_logic(fmt, ...) printf("[logic] " fmt "\r\n", ##__VA_ARGS__)

//#define LOG(fmt, ...) (void)0
#define LOG(fmt, ...) printf("[LOG] " fmt "\r\n", ##__VA_ARGS__)

//#define LOG_Debug(fmt, ...) (void)0
#define LOG_Debug(fmt, ...) printf("[%s:%d] " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
//================================================================================================


// ================================ 辅助宏 ================================
// 计算数组元素个数 
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
// Modbus 浮点缩放、32位拼接工具宏
#define FLOAT_TO_REG(f)    ((int32_t)((f) * 100.0f))
#define REG_TO_FLOAT(r)    ((float)(r) / 100.0f)
#define PACK32(hi,lo)      (((uint32_t)(hi) << 16) | (uint16_t)(lo))
#define UNPACK32(val,hi,lo) do{hi = (uint16_t)((uint32_t)(val) >> 16); lo = (uint16_t)((uint32_t)(val) & 0xFFFF);}while(0)
//================================================================================================


// ================================ 设备配置（可修改） ================================
#define HMAC_KEY    "0123456789abcdef0123456789abcdef"
//#define DEVICE_SN               "OIL-2026-0003"
//#define DEVICE_ID               "OIL-2026-0003"
#define DEVICE_MODEL            "LUB-PUMP-V1.0"
#define DEVICE_FW_VER           "1.0.3"
#define DOMAIN_NAME             "FACTORY_OIL"
#define DEVICE_HW_VER           "V4.5"  // 自定义硬件版本
//================================================================================================


#endif