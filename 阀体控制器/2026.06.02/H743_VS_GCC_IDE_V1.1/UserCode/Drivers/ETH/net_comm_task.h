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
#include "running_logic.h"

#define LOG(fmt, ...) printf("[NET] " fmt "\r\n", ##__VA_ARGS__)
#define HMAC_KEY    "0123456789abcdef0123456789abcdef"
//======================== 设备配置（可修改）========================
#define DEVICE_SN               "OIL-2026-0001"
#define DEVICE_MODEL            "LUB-CTRL-V1.0"
#define DEVICE_FW_VER           "1.0.3"
#define DEVICE_ID               "OIL_DEV_101"
#define DOMAIN_NAME             "FACTORY_OIL"

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
#define HEARTBEAT_PERIOD_MS 30000U  // 设备心跳 30s

//======================== 缓存大小 =============================
#define RX_BUF_SIZE         2048
#define UDP_MSG_QUEUE_LEN   8

#define TASK_DATA_MAX       8

#define TCP_TASK_QUEUE_LEN  16

//======================== 结构体定义 =============================
typedef struct {
    uint8_t  ip[4];//IP 地址
    uint8_t  netmask[4];// 子网掩码
    uint8_t  gateway[4];// 网关
    uint8_t  server_ip[4];// 服务器IP
    uint8_t  configured;
} NetConfig_t;

typedef struct
{
    char data[512];
    uint16_t len;
    ip_addr_t src_ip;
    uint16_t src_port;
}UdpMsgTypeDef;


// heartbeat 内数组元素结构体
typedef struct
{
    uint16_t inj_id;//注油器ID，1-8
    uint16_t inj_jr;//今日注油次数
    uint16_t inj_st;//状态：0=空闲，1=注油中，2=故障
    uint16_t inj_tr;//单次注油时间（ms）
    uint16_t inj_ts;//注油间隔（ms）
    uint16_t inj_vs;//单次注油量
}HeartCapInfo_t;

// heartbeat心跳整体入参
typedef struct
{
    uint8_t run_status;// 运行状态：0=停止，1=运行
    HeartCapInfo_t cap_arr[8];  // 8路注油器
    uint64_t last_period_seq;//最后执行的周期序号
    uint16_t last_sub_index;//最后执行的次序号
    char last_task_id[48];//最后执行的任务ID
    uint16_t last_task_result;//最后任务结果
}HeartBeatParam_t;

// task_request 单条任务信息，键名固定：inj_id / inj_v
typedef struct
{
    uint16_t inj_id;
    uint16_t inj_v;
} TaskInfo_t;

// task_order 单条子任务字段，键名固定
typedef struct
{
    char task_id[48];
    uint16_t task_type;
    uint16_t inj_id;
    uint16_t inj_v;
    uint16_t sub_index;
    uint16_t sub_total;
    uint64_t period_seq;
    uint16_t sub_volume;
    uint16_t remaining;
} TaskOrderInfo_t;

// execute_result 单条子任务字段，键名固定
typedef struct
{
    char    task_id[48];
    uint16_t inj_id;
    uint16_t inj_v;
    uint16_t execute_result;
    uint16_t sub_index;
    uint16_t sub_total;
    uint64_t period_seq;
    uint16_t actual_volume;
    uint16_t sub_remaining;
    uint64_t duration_ms;
    float    temperature;
    uint16_t voltage;
    uint8_t  voltage_change;   // false=0, true=1
    uint8_t  temp_change;      // false=0, true=1
    uint16_t error_code;
    char     error_msg[64];
} ExecuteResultInfo_t;

//======================== 参数声明 =============================
extern struct netif gnetif;     // 外部数据库句柄（系统已初始化）
extern struct fdb_kvdb net_kvdb;     // 网络独立数据库句柄
extern lan8742_Object_t LAN8742;
extern QueueHandle_t udp_msg_queue;
extern QueueHandle_t xTcpTaskQueue;
extern osMutexId_t uart_mutex;
extern char g_task_id[48];

// 外部引用系统的锁函数
extern void lock(fdb_db_t db);
extern void unlock(fdb_db_t db);
//======================== 函数声明 =============================
void net_config_init(void);
void udp_discover_task(void *arg);
void tcp_client_task(void *arg);
int8_t init_net_db(void);
void udp_echo_task(void *arg);
void tcp_send_task_request(TaskInfo_t *info_arr, uint16_t arr_len);
void tcp_send_execute_result(ExecuteResultInfo_t *info_arr, uint16_t arr_len);
void TCP_task_processing_task(void *argument);
#ifdef __cplusplus
}
#endif

#endif
