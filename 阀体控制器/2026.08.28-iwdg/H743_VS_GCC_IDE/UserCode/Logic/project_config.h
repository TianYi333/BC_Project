#ifndef __PROJECT_CONFIG_H
#define __PROJECT_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

// ================================ 打印配置 ================================
// 声明，实现在 freertos.c
const char *my_basename(const char *path);

#define LOG_ENABLE              1U      // 总开关：1全部允许，0全部静默

#define LOG_NET_ENABLE          1U      // NET网络日志
#define LOG_LOGIC_ENABLE        1U      // 业务逻辑日志
#define LOG_RTS_ENABLE          1U      // FreeRTOS相关日志
#define LOG_DEBUG_ENABLE        1U      // 全量调试日志(带文件名)

/*-------------------------- NET日志 --------------------------*/
#if (LOG_ENABLE && LOG_NET_ENABLE)
//define LOG_NET(fmt, ...)          printf("[NET] " fmt "\r\n", ##__VA_ARGS__)
#define LOG_NET(fmt, ...)       printf("[NET:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//net_comm_task.c显示行号
#else
#define LOG_NET(fmt, ...)       ((void)0)
#endif

/*-------------------------- 业务逻辑日志 --------------------------*/
#if (LOG_ENABLE && LOG_LOGIC_ENABLE)
//#define LOG_logic(fmt, ...)       printf("[LOG] " fmt "\r\n", ##__VA_ARGS__)
#define LOG_logic(fmt, ...)     printf("[LOG:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//running_logic.c显示行号
#else
#define LOG_logic(fmt, ...)     ((void)0)
#endif

/*-------------------------- RTOS日志 --------------------------*/
#if (LOG_ENABLE && LOG_RTS_ENABLE)
// #define LOG_RTS(fmt, ...)       printf("[RTS] " fmt "\r\n", ##__VA_ARGS__)
#define LOG_RTS(fmt, ...)       printf("[RTS:%d] " fmt "\r\n", __LINE__, ##__VA_ARGS__)//freertos.c显示行号
#else
#define LOG_RTS(fmt, ...)       ((void)0)
#endif

/*-------------------------- Debug 完整日志（文件名+行号） --------------------------*/
#if (LOG_ENABLE && LOG_DEBUG_ENABLE)
// #define LOG_Debug(fmt, ...)     printf("[%s:%d] " fmt "\r\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define LOG_Debug(fmt, ...)     printf("[%s:%d] " fmt "\r\n", my_basename(__FILE__), __LINE__, ##__VA_ARGS__)//只显示文件名
#else
#define LOG_Debug(fmt, ...)     ((void)0)
#endif
//================================================================================================


// ================================ 辅助宏 ================================
// 计算数组元素个数 
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
//================================================================================================


// ================================ 设备配置（可修改） ================================
#define HMAC_KEY    "0123456789abcdef0123456789abcdef"
//#define DEVICE_SN               "OIL-2026-0003"
//#define DEVICE_ID               "OIL-2026-0003"
#define DEVICE_MODEL            "LUB-CTRL-V1.0"
#define DEVICE_FW_VER           "1.0.3"
#define DOMAIN_NAME             "FACTORY_OIL"

#define INJECTOR_CNT            8
//================================================================================================

#ifdef __cplusplus
}
#endif
#endif