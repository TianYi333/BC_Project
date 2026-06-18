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
#include "lwip/apps/sntp.h"// SNTP客户端头文件
#include "lwip/dns.h"// DNS解析头文件
#include <inttypes.h>// PRIx64等格式化宏定义
#include "usart.h"

//#define LOG(fmt, ...) 0
#define LOG(fmt, ...) printf("[N] " fmt "\r\n", ##__VA_ARGS__)
#define HMAC_KEY    "0123456789abcdef0123456789abcdef"
//======================== 设备配置（可修改）========================
#define DEVICE_SN               "OIL-2026-0001"
#define DEVICE_MODEL            "LUB-CTRL-V1.0"
#define DEVICE_FW_VER           "1.0.3"
#define DEVICE_ID               "OIL-2026-0001"
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
#define HB_LOST_MAX    2    //连续2次无回复则重连

//======================== 缓存大小 =============================
#define RX_BUF_SIZE         2048
#define UDP_MSG_QUEUE_LEN   8

#define TASK_DATA_MAX       8

#define TCP_TASK_QUEUE_LEN  16

#define TASK_ID_LEN         48  // 根据实际任务ID长度定义，按需调整


//======================== 错误码 ===============================






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
    char data[512];
    uint16_t len;
    ip_addr_t src_ip;
    uint16_t src_port;
}UdpMsgTypeDef;

//设备子状态结构体
typedef struct
{
    uint8_t is_oiling;          //整机是否正在注油，0 false 1 true
    uint16_t current_inj_id;    //当前正在注油的注油器ID，0表示无
    char current_task_id[48];   //当前正在执行的任务ID，空字符串表示无
    uint64_t current_task_start;//当前任务开始时间，单位ms，Unix时间戳
    uint64_t estimated_complete;//预计完成时间，单位ms，Unix时间戳
}DevStatus_t;

// heartbeat心跳整体入参
typedef struct
{
    uint8_t run_status;         // 运行状态：0=停止，1=运行
    DevStatus_t dev_status;
} HeartBeatParam_t;

// 状态应答 state_response->injectors数组单项
typedef struct
{
    uint16_t inj_id;            //当前注油器ID，1-8
    // uint16_t inj_jr;         //今日注油次数
    uint8_t  status;            //0=正常,1=禁用,2=故障
    uint16_t inj_ts;            //注油间隔
    uint16_t inj_vs;            //单次注油量
    uint16_t last_error_code;   //最后错误代码，0表示无错误
    char last_error_msg[32];    //最后错误信息
}InjectorItem_t;

//状态应答 state_response
typedef struct
{
    uint8_t run_status;                 // 运行状态：0=停止，1=运行
    DevStatus_t dev_status;
    InjectorItem_t injector;            // 仅单个端口，不再用数组（匹配协议）
} StateRespParam_t;


// task_request 单条任务信息，键名固定：inj_id / inj_v
typedef struct
{
    uint16_t inj_id;
    uint16_t inj_v;
    uint16_t inj_ts;//注油间隔（ms）
    char     task_id[TASK_ID_LEN];  //任务ID
} TaskInfo_t;

// task_order 单条子任务字段，键名固定
typedef struct
{
    char task_id[48];
    uint16_t inj_id;
    uint16_t inj_ts;    // 新增字段：对应报文 inj_ts
    uint16_t inj_v;
} TaskOrderInfo_t;

// execute_result 单条子任务字段，键名固定
typedef struct
{
    char task_id[48];
    uint16_t inj_id;
    uint16_t execute_result;  //1成功 0失败
    uint16_t actual_volume;

    uint16_t valve_act_tm;     // 阀体单次工作耗时(ms)
    uint16_t valve_total_tm;   // 阀体工作总耗时(ms)
    uint16_t valve_temp;       // 阀体温度(℃)
    uint16_t valve_hum;        // 阀体湿度(RH%)

    uint16_t error_code;
    char error_msg[32];
}ExecuteResultInfo_t;

//======================== 参数声明 =============================
extern struct netif gnetif;     // 外部数据库句柄（系统已初始化）
extern struct fdb_kvdb net_kvdb;     // 网络独立数据库句柄
extern lan8742_Object_t LAN8742;
extern QueueHandle_t udp_msg_queue;
extern QueueHandle_t xTcpTaskQueue;
extern QueueHandle_t xTimerReqQueue;
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
void tcp_send_task_request(TaskInfo_t *info_arr, uint16_t arr_len, uint8_t is_oiling);
void tcp_send_execute_result(ExecuteResultInfo_t *info_arr, uint16_t arr_len);
void TCP_task_processing_task(void *argument);
void tcp_send_state_response(StateRespParam_t *p_param);
void timer_req_send_task(void *arg);
void TimeSyncTask(void *arg);
void test_oil_filling_task(void *arg);
#ifdef __cplusplus
}
#endif

#endif
