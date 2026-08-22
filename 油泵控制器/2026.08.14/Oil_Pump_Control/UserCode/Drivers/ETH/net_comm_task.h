#ifndef __NET_COMM_TASK_H
#define __NET_COMM_TASK_H

#ifdef __cplusplus
extern "C" {
#endif
#include "FreeRTOS.h"
#include "queue.h"
#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/igmp.h"// 加入IGMP头文件以支持组播
#include "lwip/netif.h"
#include "cmsis_os.h"
#include "cJSON.h"
#include "string.h"
#include "stdio.h"
#include <stdint.h>
#include "rtc_clock.h"  // 引入RTC相关函数声明
#include "HMAC-SHA256.h"
#include "flashdb.h"// FlashDB 头文件
#include "ethernetif.h"   // 包含LAN8742驱动的头文件
#include "LAN8742.h"      // 如果工程里有单独的PHY驱动头文件，也加上
#include "lwip/autoip.h" //启用 169.254.x.x 本地链路 IP 功能
#include "timers.h"// FreeRTOS定时器头文件
#include "lwip/apps/sntp.h"// SNTP客户端头文件
#include "lwip/dns.h"// DNS解析头文件
#include <inttypes.h>// PRIx64等格式化宏定义
#include "usart.h"
#include "sht40.h"
#include "main_logic.h"

#define LOG_ENABLE  1   // 1: 开启打印, 0: 关闭打印

#if LOG_ENABLE
#define LOG_NET(fmt, ...) printf("[NET] " fmt "\r\n", ##__VA_ARGS__)
#else
#define LOG_NET(fmt, ...) ((void)0)
#endif

// 辅助宏：计算数组元素个数
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define HMAC_KEY    "0123456789abcdef0123456789abcdef"
//======================== 设备配置（可修改）========================
//#define DEVICE_SN               "OIL-2026-0003"
#define DEVICE_MODEL            "LUB-PUMP-V1.0"
#define DEVICE_FW_VER           "1.0.3"
//#define DEVICE_ID               "OIL-2026-0003"
#define DOMAIN_NAME             "FACTORY_OIL"
#define DEVICE_HW_VER           "V2.1"  // 自定义硬件版本

#define INJECTOR_CNT            8
#define INJ_DEFAULT_JR          6
#define INJ_DEFAULT_ST          0
#define INJ_DEFAULT_TR          800
#define INJ_DEFAULT_TS          1200
#define INJ_DEFAULT_VS          10

//======================== 以太网 ================================
#define UDP_LISTEN_PORT     50000
#define TCP_SERVER_PORT     50010

#define BROADCAST_ADDR      "255.255.255.255"
#define MULTICAST_ADDR      "239.255.100.100"

//======================== 安全参数 =============================
#define TIME_VALID_SEC      60  // 时间戳 ±60s
#define REQ_TIMEOUT_MS      2000U   // 单次请求超时 2s
#define HEARTBEAT_PERIOD_MS 20000U  // 设备心跳 30s
#define HB_LOST_MAX         2    //连续2次无回复则重连

//======================== 缓存大小 =============================
#define RX_BUF_SIZE         2048
#define UDP_MSG_QUEUE_LEN   8

#define TASK_DATA_MAX       8

#define TCP_TASK_QUEUE_LEN  16

#define TASK_ID_LEN         48  // 根据实际任务ID长度定义，按需调整


//======================== 错误码 ===============================



//========================重启掉电存储参数 ===============================
#define REBOOT_MAGIC_NUM    0x52454254  // ASCII: "REBT"，固定校验标记
#define KV_KEY_REBOOT_INFO  "reboot_info" // KV节点名

//======================== 结构体定义 =============================
typedef struct {
    uint8_t  ip[4];         //IP地址
    uint8_t  netmask[4];    //子网掩码
    uint8_t  gateway[4];    //网关
    uint8_t  server_ip[4];  //服务器IP
    uint8_t  configured;
} NetConfig_t;

typedef struct
{
    char data[1500];
    uint16_t len;
    ip4_addr_t src_ip;
    uint16_t src_port;
}UdpMsgTypeDef;

// heartbeat心跳整体入参
typedef struct
{
    SYS_WORK_STATE_E    sys_start;          //整机工作状态 _SYS_STATUS.sys_start
    uint8_t             sys_warning;        //系统告警：0无警告，1有警告
    SENSOR_STATUS_E     sensor_status;      //注意协议：1就绪，0故障，核对枚举定义
    uint8_t             sys_fault_bit;      //整机故障位图
    int32_t             pressure_mpa;       //压力 ×100；adc_pressure *100
    int32_t             oil_temp_c;        //油温 ×100；adc_oil_temp *100
    uint32_t            liquid_level_pct;  //液位百分比 ×100
    uint8_t             valve_state;        //阀芯 0关闭 1打开
} HeartBeatParam_t;

// state_response query_type=1 设备配置参数，全部放大×100
typedef struct
{
    int32_t ref_pressure;         //基准压力*100
    int32_t max_pressure;        //最高保护压力*100
    int32_t press_hysteresis;     //压力回差*100
    int32_t overpress_margin;     //超压裕量*100

    int32_t max_motor_speed;      //最大转速 RPM
    int32_t min_motor_speed;      //最小转速 RPM

    int32_t max_oil_temp;         //最高油温保护*100
    int32_t min_liquid_level;    //最低液位保护*100

    int32_t pid_kp;               //kp*100
    int32_t pid_ki;               //ki*100
    int32_t pid_kd;               //kd*100
} DeviceConfigParam_t;

// state_response 应答入参
typedef struct
{
    uint8_t query_type;     //0=运行状态 1=配置
    HeartBeatParam_t dev_status;
    DeviceConfigParam_t dev_cfg;
} StateRespParam_t;

// 重启指令参数结构体
typedef struct {
    uint32_t delay_sec;    // 延迟重启秒数
    char reason[64];       // 重启原因
} RebootCmd_t;

// 重启持久化信息结构体，单Blob存入net_kvdb
typedef struct {
    uint32_t magic;
    char reason[64];
} RebootPersistentInfo_t;

// 发送队列消息：动态分配的字符串指针
typedef struct {
    char *data;   // 待发送的 JSON 字符串（以 '\0' 结尾）
} tcp_send_msg_t;


//======================== 参数声明 =============================
extern struct netif gnetif;     // 外部数据库句柄（系统已初始化）
extern struct fdb_kvdb net_kvdb;     // 网络独立数据库句柄
extern NetConfig_t g_net_cfg;   
extern lan8742_Object_t LAN8742;
extern QueueHandle_t udp_msg_queue;
extern QueueHandle_t xRebootCmdQueue;
extern QueueHandle_t tcp_send_queue;
extern osMutexId_t uart_mutex;
extern osMutexId_t flash_kv_mutex;
extern osMutexId_t udp_pcb_mutex;
extern osMutexId_t tcp_send_mutex;
extern uint8_t g_current_running_inj_id;// 全局记录当前正在执行注油的出油口ID，0=无任务运行
extern uint32_t Initialization_time;//系统初始化时间，单位ms
// 温湿度全局缓存，上报统一放大10倍整型
extern uint16_t g_sht40_temp_cache;
extern uint16_t g_sht40_hum_cache;
// 外部引用系统的锁函数
extern void lock(fdb_db_t db);
extern void unlock(fdb_db_t db);
#define DEV_SN_STR_LEN  24
extern char g_device_sn[DEV_SN_STR_LEN];
extern char g_device_id[DEV_SN_STR_LEN];
//======================== 函数声明 =============================
void GenerateDeviceSNFromUID(void);
void net_config_save(void);
void net_config_init(void);
void Udp_discover_task(void *arg);
void Tcp_client_task(void *arg);
int8_t init_net_db(void);
void TimeSyncTask(void *arg);
void vRebootTask(void *pvParameters);
void tcp_sender_task(void *arg);
int8_t init_reboot_db(void);
#ifdef __cplusplus
}
#endif

#endif
