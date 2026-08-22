/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : This file overrides LwIP stack default configuration
  *                      done in opt.h file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion --------------------------------------*/
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

/*-----------------------------------------------------------------------------*/
/* Current version of LwIP supported by CubeMx: 2.1.2 -*/
/*-----------------------------------------------------------------------------*/

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */
#define LWIP_DHCP                   0
// 开启AutoIP（169.254.x.x本地链路IP）
#define LWIP_AUTOIP                 1

#define AUTOIP_TMR_INTERVAL         100
// 关闭DHCP与AutoIP联动
#define LWIP_DHCP_AUTOIP_COOP       0
// 开启 IGMP 组播
#define LWIP_IGMP                   0
#define LWIP_BROADCAST              1

//开启 lwip TCP 保活
#define LWIP_TCP_KEEPALIVE          0
#define TCP_KEEPIDLE_DEFAULT        10000UL
#define TCP_KEEPINTVL_DEFAULT       3000UL
#define TCP_KEEPCNT_DEFAULT         5U

#define TCP_MSS              1460u         // 可选，确保 MSS 明确
#define TCP_SND_BUF          (16 * 1024)   // 16KB，根据内存余量可改为 4KB 或 6KB
#define TCP_WND              (16 * 1024)   // 接收窗口也可适当增大，非必须
//#define TCP_SND_QUEUELEN     (4 * TCP_SND_BUF / TCP_MSS) // 标准计算公式，自动计算队列深度
#define LWIP_TCP_NODELAY     1U  // 关闭Nagle算法，移除tcp_output后消除小包延迟

#define MEMP_NUM_TCP_SEG        64
#define MEMP_NUM_UDP_PCB        12     // 增加UDP PCB数量
#define MEMP_NUM_TCP_PCB        20

#define LWIP_MEMPOOL_STATS      1

#define LWIP_SNTP               1
#define SNTP_MAX_SERVERS        3
#define SNTP_STARTUP_DELAY      0    //网卡就绪立刻校时
#define SNTP_SUPPRESS_DELAY_CHECK 1
#define SNTP_UPDATE_DELAY       (15*1000) //15秒同步一次
extern uint64_t sys_unix_ms;

//不加1900偏移，仅做时区+毫秒拼接，适配当前PC NTP服务
/*sec 已经是北京时间，不再额外加8小时*/
#define SNTP_SET_SYSTEM_TIME_US(sec, us) do{ \
    uint64_t sec_cn = (uint64_t)(sec); \
    sys_unix_ms = sec_cn * 1000ULL + ((uint64_t)us / 1000ULL); \
}while(0)

/*//sec加8小时偏移 
// #define SNTP_SET_SYSTEM_TIME_US(sec, us) do{ \
//     uint64_t sec_cn = (uint64_t)(sec) + 8UL*3600UL; \
//     sys_unix_ms = sec_cn * 1000ULL + ((uint64_t)us / 1000ULL); \
// }while(0)
*/

// 开启TCPIP内核锁，多任务并发安全增强
#define LWIP_TCPIP_CORE_LOCKING 1
/* USER CODE END 0 */

#ifdef __cplusplus
 extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- WITH_RTOS enabled (Since FREERTOS is set) -----*/
#define WITH_RTOS 1
/* Temporary workaround to avoid conflict on errno defined in STM32CubeIDE and lwip sys_arch.c errno */
#undef LWIP_PROVIDE_ERRNO
/*----- CHECKSUM_BY_HARDWARE enabled -----*/
#define CHECKSUM_BY_HARDWARE 1
/*-----------------------------------------------------------------------------*/

/* LwIP Stack Parameters (modified compared to initialization value in opt.h) -*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- Default value in ETH configuration GUI in CubeMx: 1524 -----*/
#define ETH_RX_BUFFER_SIZE 1536
/*----- Default Value for MEM_LIBC_MALLOC: 0 ---*/
#define MEM_LIBC_MALLOC 1
/*----- Default Value for MEMP_MEM_MALLOC: 0 ---*/
#define MEMP_MEM_MALLOC 1
/*----- Default Value for MEMP_MEM_INIT: 0 ---*/
#define MEMP_MEM_INIT 1
/*----- Value in opt.h for MEM_ALIGNMENT: 1 -----*/
#define MEM_ALIGNMENT 4
/*----- Default Value for MEM_SIZE: 1600 ---*/
#define MEM_SIZE 65535
/*----- Default Value for MEMP_USE_CUSTOM_POOLS: 0 ---*/
#define MEMP_USE_CUSTOM_POOLS 1
/*----- Default Value for LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT: 0 ---*/
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT 1
/*----- Default Value for H7 devices: 0x30004000 -----*/
#define LWIP_RAM_HEAP_POINTER 0x30000800
/*----- Default Value for MEMP_NUM_PBUF: 16 ---*/
#define MEMP_NUM_PBUF 64
/*----- Value supported for H7 devices: 1 -----*/
#define LWIP_SUPPORT_CUSTOM_PBUF 1
/*----- Default Value for PBUF_POOL_SIZE: 16 ---*/
#define PBUF_POOL_SIZE 128
/*----- Value in opt.h for LWIP_ETHERNET: LWIP_ARP || PPPOE_SUPPORT -*/
#define LWIP_ETHERNET 1
/*----- Value in opt.h for LWIP_DNS_SECURE: (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT) -*/
#define LWIP_DNS_SECURE 7
/*----- Default Value for TCP_SND_QUEUELEN: 9 ---*/
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS) // 标准计算公式，自动计算队列深度
/*----- Value in opt.h for TCP_SNDLOWAT: LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (TCP_SND_BUF) - 1) -*/
#define TCP_SNDLOWAT 1071
/*----- Value in opt.h for TCP_WND_UPDATE_THRESHOLD: LWIP_MIN(TCP_WND/4, TCP_MSS*4) -----*/
#define TCP_WND_UPDATE_THRESHOLD 536
/*----- Value in opt.h for LWIP_NETIF_LINK_CALLBACK: 0 -----*/
#define LWIP_NETIF_LINK_CALLBACK 1
/*----- Value in opt.h for TCPIP_THREAD_STACKSIZE: 0 -----*/
#define TCPIP_THREAD_STACKSIZE 4096
/*----- Value in opt.h for TCPIP_THREAD_PRIO: 1 -----*/
#define TCPIP_THREAD_PRIO 32
/*----- Value in opt.h for TCPIP_MBOX_SIZE: 0 -----*/
#define TCPIP_MBOX_SIZE 6
/*----- Value in opt.h for SLIPIF_THREAD_STACKSIZE: 0 -----*/
#define SLIPIF_THREAD_STACKSIZE 1024
/*----- Value in opt.h for SLIPIF_THREAD_PRIO: 1 -----*/
#define SLIPIF_THREAD_PRIO 28
/*----- Value in opt.h for DEFAULT_THREAD_STACKSIZE: 0 -----*/
#define DEFAULT_THREAD_STACKSIZE 1024
/*----- Value in opt.h for DEFAULT_THREAD_PRIO: 1 -----*/
#define DEFAULT_THREAD_PRIO 28
/*----- Value in opt.h for DEFAULT_UDP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_UDP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_TCP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_TCP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_ACCEPTMBOX_SIZE: 0 -----*/
#define DEFAULT_ACCEPTMBOX_SIZE 6
/*----- Default Value for LWIP_NETCONN_FULLDUPLEX: 0 ---*/
#define LWIP_NETCONN_FULLDUPLEX 1
/*----- Value in opt.h for RECV_BUFSIZE_DEFAULT: INT_MAX -----*/
#define RECV_BUFSIZE_DEFAULT 2000000000
/*----- Default Value for LWIP_SNTP: 0 ---*/
#define LWIP_SNTP 1
/*----- Value in opt.h for LWIP_STATS: 1 -----*/
#define LWIP_STATS 1
/*----- Value in opt.h for CHECKSUM_GEN_IP: 1 -----*/
#define CHECKSUM_GEN_IP 0
/*----- Value in opt.h for CHECKSUM_GEN_UDP: 1 -----*/
#define CHECKSUM_GEN_UDP 0
/*----- Value in opt.h for CHECKSUM_GEN_TCP: 1 -----*/
#define CHECKSUM_GEN_TCP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP6: 1 -----*/
#define CHECKSUM_GEN_ICMP6 0
/*----- Value in opt.h for CHECKSUM_CHECK_IP: 1 -----*/
#define CHECKSUM_CHECK_IP 0
/*----- Value in opt.h for CHECKSUM_CHECK_UDP: 1 -----*/
#define CHECKSUM_CHECK_UDP 0
/*----- Value in opt.h for CHECKSUM_CHECK_TCP: 1 -----*/
#define CHECKSUM_CHECK_TCP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP6: 1 -----*/
#define CHECKSUM_CHECK_ICMP6 0
/*-----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /*__LWIPOPTS__H__ */
