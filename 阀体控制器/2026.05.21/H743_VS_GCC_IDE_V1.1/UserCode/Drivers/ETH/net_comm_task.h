#ifndef __NET_COMM_TASK_H
#define __NET_COMM_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/igmp.h"// 加入IGMP头文件以支持组播
#include "cmsis_os.h"
#include "cJSON.h"
#include "string.h"
#include "stdio.h"
#include <stdint.h>
#include "rtc_clock.h"  // 引入RTC相关函数声明
#include "HMAC-SHA256.h"
#include "flashdb.h"// FlashDB 头文件


#define LOG(fmt, ...) printf("[NET] " fmt "\r\n", ##__VA_ARGS__)

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
#define UDP_LISTEN_PORT     9527
#define TCP_SERVER_PORT     9601
#define TCP_CONFIG_PORT     9530    // 配网TCP服务端

#define BROADCAST_ADDR      "255.255.255.255"
#define MULTICAST_ADDR      "239.255.100.100"

//======================== 安全参数 =============================
#define TIME_VALID_SEC      60  // 时间戳 ±60s
#define REQ_TIMEOUT_MS      2000U   // 单次请求超时 2s
#define HEARTBEAT_PERIOD_MS 30000U  // 设备心跳 30s

//======================== 缓存大小 =============================
#define RX_BUF_SIZE         2048

//======================== 配网参数结构体 =============================
typedef struct {
    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  gateway[4];
    uint8_t  configured;
} NetConfig_t;

//======================== 参数声明 =============================
extern struct netif gnetif;     // 外部数据库句柄（系统已初始化）
extern struct fdb_kvdb net_kvdb;     // 网络独立数据库句柄

// 外部引用系统的锁函数
extern void lock(void);
extern void unlock(void);
//======================== 函数声明 =============================
void net_config_init(void);
void udp_discover_task(void *arg);
void tcp_client_task(void *arg);
void tcp_config_server_task(void *arg); //配网TCP服务端任务
int8_t init_net_db(void);


#ifdef __cplusplus
}
#endif

#endif
