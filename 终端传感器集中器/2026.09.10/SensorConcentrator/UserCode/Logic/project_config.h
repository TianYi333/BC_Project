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

// 声明，实现在 freertos.c
const char *my_basename(const char *path);
// ================================ 打印配置 ================================
//#define LOG_NET(fmt, ...) ((void)0)
//define LOG_NET(fmt, ...) printf("[NET] " fmt "\r\n", ##__VA_ARGS__)
#define LOG_NET(fmt, ...) printf("[NET:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//显示行号

#define LOG_MB(fmt, ...) (void)0
//#define LOG_MB(fmt, ...) printf("[MBS] " fmt "\r\n", ##__VA_ARGS__)
//#define LOG_MB(fmt, ...) printf("[MBS:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//显示行号

#define LOG_ADC(fmt, ...) (void)0
//#define LOG_ADC(fmt, ...) printf("[ADC] " fmt "\r\n", ##__VA_ARGS__)
//#define LOG_ADC(fmt, ...) printf("[ADC:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//显示行号

//#define LOG_logic(fmt, ...) (void)0
//#define LOG_logic(fmt, ...) printf("[LOG] " fmt "\r\n", ##__VA_ARGS__)
#define LOG_logic(fmt, ...) printf("[LOG:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//显示行号

//#define LOG_RTS(fmt, ...) (void)0
//#define LOG_RTS(fmt, ...) printf("[RTS] " fmt "\r\n", ##__VA_ARGS__)
#define LOG_RTS(fmt, ...) printf("[RTS:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//显示行号

//#define LOG_Debug(fmt, ...) (void)0
//#define LOG_Debug(fmt, ...) printf("[%s:%d] " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define LOG_Debug(fmt, ...) printf("[%s:%d] " fmt "\r\n", my_basename(__FILE__), __LINE__, ##__VA_ARGS__)//只显示文件名

//#define LOG_OTA(fmt, ...) (void)0
//#define LOG_OTA(...) printf(__VA_ARGS__)
#define LOG_OTA(fmt, ...) printf("[OTA:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//显示行号
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
#define DEVICE_MODEL            "SENSOR-HUB-V1.0"
#define DEVICE_FW_VER           "1.0.0"
#define DOMAIN_NAME             "FACTORY_OIL"
#define DEVICE_HW_VER           "V1.0"  // 自定义硬件版本
//================================================================================================


#endif