#ifndef __NET_COMM_TASK_H
#define __NET_COMM_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "project_config.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "lwip.h"
#include "lwip/tcpip.h"
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
#include "lwip/netifapi.h" // netifapi_* 安全接口：在非 tcpip_thread 上下文调用，自动 marshal 到 tcpip_thread 执行（满足 LWIP_ASSERT_CORE_LOCKED）
#include "timers.h"// FreeRTOS定时器头文件
#include "lwip/apps/sntp.h"// SNTP客户端头文件
#include "lwip/dns.h"// DNS解析头文件
#include <inttypes.h>// PRIx64等格式化宏定义
#include "usart.h"


//======================== 以太网 ================================
#define UDP_LISTEN_PORT     50000
#define TCP_SERVER_PORT     50010

#define BROADCAST_ADDR      "255.255.255.255"
#define MULTICAST_ADDR      "239.255.100.100"

//======================== 安全参数 =============================
#define TIME_VALID_SEC      60  // 时间戳 ±60s
#define REQ_TIMEOUT_MS      2000U   // 单次请求超时 2s
// 应用层心跳已移除（HEARTBEAT_PERIOD_MS / HB_LOST_MAX 随之废弃，勿再使用）。
// 连接存活检测改为两层：
//   1) 物理断线 → ethernetif.c 链路线程 PHY 轮询 + net_comm_task.c 里 netif_is_link_up() 边沿检测（100ms 级）
//   2) 链路仍 up 但对端已死 → lwIP TCP Keepalive（见 lwipopts.h：
//      LWIP_TCP_KEEPALIVE=1、TCP_KEEPIDLE_DEFAULT=10s、TCP_KEEPINTVL_DEFAULT=3s、TCP_KEEPCNT_DEFAULT=5，
//      即空闲 10s 起探测、间隔 3s、连续 5 次无响应则 abort 触发重连，总超时 25s）

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

// 重启指令参数结构体
typedef struct {
    uint32_t delay_sec;    // 延迟重启秒数
    char reason[64];       // 重启原因
} RebootCmd_t;

// 重启持久化信息结构体，单Blob存入共享 kvdb（内部Flash）
typedef struct {
    uint32_t magic;
    char reason[64];
} RebootPersistentInfo_t;

//======================== 传感器模式/状态定义 =============================
#define SENSOR_MODE_REG        0   // 寄存器模式
#define SENSOR_MODE_ACTIVE     1   // 主动上报模式

// mode_set_resp / mode_set_req 状态枚举
#define MODE_SET_OK            0   // 成功
#define MODE_SET_INVALID       1   // 无效模式
#define MODE_SET_WRITE_FAIL    2   // 写入失败
#define MODE_SET_OFFLINE       3   // 传感器离线

// 传感器数据上报载荷（集中器 → 网关）
typedef struct {
    uint8_t  sensor_id;     // 传感器ID（1-8）
    int32_t  temperature;   // 温度值，放大100倍，如 2550 = 25.50℃
    int32_t  voltage;       // 磁传感器电压（mV），0~3300
    uint8_t  voltage_flag;  // 电压突变标志：0-无，1-有
    uint8_t  temp_flag;     // 温度突变标志：0-无，1-有
    uint8_t  mode;          // 当前工作模式：0-寄存器模式，1-主动上报模式
} SensorData_t;

// 发送队列消息：动态分配的字符串指针
typedef struct {
    char *data;   // 待发送的 JSON 字符串（以 '\0' 结尾）
} tcp_send_msg_t;


//======================== 参数声明 =============================
extern struct netif gnetif;     // 外部数据库句柄（系统已初始化）
/* 网络配置使用 syncif.c 的共享 kvdb（内部Flash），见 syncif.h */
extern NetConfig_t g_net_cfg;   
extern lan8742_Object_t LAN8742;
extern QueueHandle_t udp_msg_queue;
extern QueueHandle_t xRebootCmdQueue;
extern QueueHandle_t tcp_send_queue;
extern SemaphoreHandle_t uart_mutex;
extern SemaphoreHandle_t flash_kv_mutex;
extern SemaphoreHandle_t udp_pcb_mutex;
extern SemaphoreHandle_t tcp_send_mutex;
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
void Udp_discover_task(const void * argument);
void Tcp_client_task(const void * argument);
int8_t init_net_db(void);
void TimeSyncTask(const void * argument);
void vRebootTask(const void * argument);
void tcp_sender_task(const void * argument);
void tcp_send_motor_event_report(uint8_t event, uint64_t tick_ts);
// 传感器数据上报（集中器中断收到终端传感器数据后，由任务上下文调用）
void tcp_send_sensor_data(const SensorData_t *sd);
// 读取某传感器本地缓存的工作模式（0=寄存器模式，1=主动上报模式）
uint8_t sensorhub_get_mode(uint8_t sensor_id);
#ifdef __cplusplus
}
#endif

#endif
