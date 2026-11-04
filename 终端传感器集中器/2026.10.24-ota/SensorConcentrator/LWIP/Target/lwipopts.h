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
// 作用：替代原应用层心跳包。检测职责重新划分——
//   · 物理断线（拔网线/对端掉电）→ 由 ethernet_link_thread 的 PHY 轮询 + netif 边沿检测负责（100ms 级）
//   · "链路还通但对端已死"（上位机进程崩溃/不再响应）→ 由 TCP Keepalive 负责
// 保活总超时 = keep_idle + keep_cnt * keep_intvl = 10s + 5*3s = 25s，
// 超时后 lwIP 走 tcp_abort -> errf(tcp_error_callback) -> tcp_pending_close -> 自动重连。
#define LWIP_TCP_KEEPALIVE          1
#define TCP_KEEPIDLE_DEFAULT        10000UL
#define TCP_KEEPINTVL_DEFAULT       3000UL
#define TCP_KEEPCNT_DEFAULT         5U

#define TCP_MSS              1460u         // 可选，确保 MSS 明确
#define TCP_SND_BUF          (16 * 1024)   // 16KB，根据内存余量可改为 4KB 或 6KB
#define TCP_WND              (16 * 1024)   // 接收窗口也可适当增大，非必须
//#define TCP_SND_QUEUELEN     (4 * TCP_SND_BUF / TCP_MSS) // 标准计算公式，自动计算队列深度
#define LWIP_TCP_NODELAY     1U  // 关闭Nagle算法，移除tcp_output后消除小包延迟

#define MEMP_NUM_UDP_PCB        12     // 增加UDP PCB数量
#define MEMP_NUM_TCP_PCB        20
// 扩容 SYS_TIMEOUT 池：默认值 = LWIP_NUM_SYS_TIMEOUT_INTERNAL（=TCP+ARP+AUTOIP 等内部定时器），
// 但该计算式不含 SNTP。SNTP(sntp_init) 运行时会额外占用 1~2 个 sys_timeout，再加上 TCP 连接
// (tcp_connect 触发 tcp_timer_needed 再注册 1 个) → 上电/连服务器时池被占满，第 4 个 sys_timeout
// 分配失败 → 断言 "sys_timeout: timeout != NULL, pool MEMP_SYS_TIMEOUT is empty"。
// 给足余量（内部约 3 + SNTP 2 + TCP 1 + 后续扩展）。
#define MEMP_NUM_SYS_TIMEOUT    12

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

// 开启 netifapi：允许在非 tcpip_thread 上下文（如 StartDefaultTask）安全地调用
// netif_set_*/autoip_start/stop，内部自动 marshal 到 tcpip_thread 执行，避免错上下文
// 调用导致的并发破坏（本项目 autoip_start 在普通任务里调用曾引发 HardFault）。
#define LWIP_NETIF_API 1

// 重定义 IP 地址复制宏：强制拆 u16_t 两次复制，禁止编译器将 4 字节 memcpy 优化成
// u32_t 直接 str/ldr。
// 原因：struct etharp_hdr.sipaddr/dipaddr 协议偏移为 14/24 字节，而 pbuf_alloc(PBUF_LINK)
// 返回的 payload 被按 4 字节对齐，导致这两个字段落在 2 字节对齐（非 4 字节）地址。
// 一旦 SCB->CCR.UNALIGN_TRP=1，etharp_raw 里的 u32_t 写就会触发 UsageFault→HardFault
//（仅在 AutoIP 启动后周期性发 ARP probe/announce 时走到，故屏蔽 autoip 不崩）。
/*
 * 注意：语义必须与 stock lwIP 的 `SMEMCPY(dest, src, 4)` 完全等价，即**纯字节拷贝**，
 * 绝不能加 PP_HTONS/PP_NTOHS 之类的字节序转换！
 * 原因：ip4_addr_t.addr 本身已按网络字节序存储（小端 CPU 上其内存字节即 C0 A8 00 78），
 * 而 addrw[0]/addrw[1] 是 u16_t，小端下天然拼回同样的字节序列，直接赋值即可。
 * 若多套一层 PP_HTONS，会把 ARP 报文里的 IP 反转（192.168.0.120 -> 168.192.120.0），
 * 导致：① etharp_input 解出的 dipaddr 与本端 IP 比对失败 -> for_us=0 -> **设备永不回 ARP**；
 *       ② 设备发出的 ARP request/announce 里 IP 也是错的 -> 对端无法解析设备。
 * 表现为"广播通、单播不通、ping 不通、TCP 连不上"（广播不需要 ARP，所以唯一能通）。
 *
 * 字节序验证（192.168.0.120）：
 *   网络序字节 = C0 A8 00 78 -> addr(小端读数) = 0x7800A8C0
 *   FROM: addrw[0]=(u16_t)0x7800A8C0=0xA8C0 -> 小端写出 C0 A8 ✓
 *         addrw[1]=(u16_t)(0x7800A8C0>>16)=0x7800 -> 小端写出 00 78 ✓
 *   TO  : addrw[0]=0xA8C0, addrw[1]=0x7800 -> (0x7800<<16)|0xA8C0 = 0x7800A8C0 ✓
 */
#define IPADDR_WORDALIGNED_COPY_FROM_IP4_ADDR_T(dest, src) do { \
    u32_t _tmp = ((const ip4_addr_t *)(src))->addr; \
    ((struct ip4_addr_wordaligned *)(dest))->addrw[0] = (u16_t)(_tmp); \
    ((struct ip4_addr_wordaligned *)(dest))->addrw[1] = (u16_t)(_tmp >> 16U); \
} while(0)

#define IPADDR_WORDALIGNED_COPY_TO_IP4_ADDR_T(dest, src) do { \
    u32_t _tmp; \
    _tmp  = ((u32_t)((const struct ip4_addr_wordaligned *)(src))->addrw[1]) << 16U; \
    _tmp |= (u32_t)((const struct ip4_addr_wordaligned *)(src))->addrw[0]; \
    ((ip4_addr_t *)(dest))->addr = _tmp; \
} while(0)
/* USER CODE END 0 */

#ifdef __cplusplus
 extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- WITH_RTOS enabled (Since FREERTOS is set) -----*/
#define WITH_RTOS 1
/*----- CHECKSUM_BY_HARDWARE enabled -----*/
#define CHECKSUM_BY_HARDWARE 1
/*-----------------------------------------------------------------------------*/

/* LwIP Stack Parameters (modified compared to initialization value in opt.h) -*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- Value in opt.h for MEM_ALIGNMENT: 1 -----*/
#define MEM_ALIGNMENT 4
/*----- Default Value for MEM_SIZE: 1600 ---*/
#define MEM_SIZE 24*1024
/*----- Default Value for F7 devices: 0x20048000 -----*/
#define LWIP_RAM_HEAP_POINTER 0x00000000
/*----- Default Value for MEMP_NUM_PBUF: 16 ---*/
#define MEMP_NUM_PBUF 64
/*----- Default Value for MEMP_NUM_TCP_SEG: 16 ---*/
#define MEMP_NUM_TCP_SEG 24
/*----- Default Value for PBUF_POOL_SIZE: 16 ---*/
#define PBUF_POOL_SIZE 32
/*----- Value in opt.h for LWIP_ETHERNET: LWIP_ARP || PPPOE_SUPPORT -*/
#define LWIP_ETHERNET 1
/*----- Value in opt.h for LWIP_DNS_SECURE: (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT) -*/
#define LWIP_DNS_SECURE 7
/*----- Default Value for TCP_SND_QUEUELEN: 9 ---*/
#define TCP_SND_QUEUELEN 24
/*----- Value in opt.h for TCP_SNDLOWAT: LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (TCP_SND_BUF) - 1) -*/
#define TCP_SNDLOWAT 1071
/*----- Value in opt.h for TCP_WND_UPDATE_THRESHOLD: LWIP_MIN(TCP_WND/4, TCP_MSS*4) -----*/
#define TCP_WND_UPDATE_THRESHOLD 536
/*----- Value in opt.h for LWIP_NETIF_LINK_CALLBACK: 0 -----*/
#define LWIP_NETIF_LINK_CALLBACK 1
/*----- Value in opt.h for TCPIP_THREAD_STACKSIZE: 0 -----*/
#define TCPIP_THREAD_STACKSIZE 4096
/*----- Value in opt.h for TCPIP_THREAD_PRIO: 1 -----*/
#define TCPIP_THREAD_PRIO osPriorityNormal
/*----- Value in opt.h for TCPIP_MBOX_SIZE: 0 -----*/
#define TCPIP_MBOX_SIZE 6
/*----- Value in opt.h for SLIPIF_THREAD_STACKSIZE: 0 -----*/
#define SLIPIF_THREAD_STACKSIZE 1024
/*----- Value in opt.h for SLIPIF_THREAD_PRIO: 1 -----*/
#define SLIPIF_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_THREAD_STACKSIZE: 0 -----*/
#define DEFAULT_THREAD_STACKSIZE 2048
/*----- Value in opt.h for DEFAULT_THREAD_PRIO: 1 -----*/
#define DEFAULT_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_UDP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_UDP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_TCP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_TCP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_ACCEPTMBOX_SIZE: 0 -----*/
#define DEFAULT_ACCEPTMBOX_SIZE 6
/*----- Value in opt.h for RECV_BUFSIZE_DEFAULT: INT_MAX -----*/
#define RECV_BUFSIZE_DEFAULT 2000000000
/*----- Default Value for LWIP_SNTP: 0 ---*/
#define LWIP_SNTP 1
/*----- Default Value for LWIP_TFTP: 0 ---*/
#define LWIP_TFTP 1
/*----- Value in opt.h for LWIP_STATS: 1 -----*/
#define LWIP_STATS 0
/*----- Value in opt.h for CHECKSUM_GEN_IP: 1 -----*/
#define CHECKSUM_GEN_IP 0
/*----- Value in opt.h for CHECKSUM_GEN_UDP: 1 -----*/
#define CHECKSUM_GEN_UDP 0
/*----- Value in opt.h for CHECKSUM_GEN_TCP: 1 -----*/
#define CHECKSUM_GEN_TCP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP: 1 -----*/
#define CHECKSUM_GEN_ICMP 0
/*----- Value in opt.h for CHECKSUM_GEN_ICMP6: 1 -----*/
#define CHECKSUM_GEN_ICMP6 0
/*----- Value in opt.h for CHECKSUM_CHECK_IP: 1 -----*/
#define CHECKSUM_CHECK_IP 0
/*----- Value in opt.h for CHECKSUM_CHECK_UDP: 1 -----*/
#define CHECKSUM_CHECK_UDP 0
/*----- Value in opt.h for CHECKSUM_CHECK_TCP: 1 -----*/
#define CHECKSUM_CHECK_TCP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP: 1 -----*/
#define CHECKSUM_CHECK_ICMP 0
/*----- Value in opt.h for CHECKSUM_CHECK_ICMP6: 1 -----*/
#define CHECKSUM_CHECK_ICMP6 0
/*-----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */
/* 修复 LwIP 内部堆锚死在 0x20020000 与 FreeRTOS ucHeap 重叠的问题：
 * CubeMX 为 F7 生成的默认 LWIP_RAM_HEAP_POINTER=0x20020000 正好落在
 * ucHeap(0x2001097C~0x2002997C) 范围内，插网线收包后 LwIP 堆与任务栈
 * 互相踩踏，表现为 STKOF / AutoIP 协商失败。取消该绝对地址，让 LwIP
 * 在 .bss 内正规分配 ram_heap 数组。 */
#undef LWIP_RAM_HEAP_POINTER

/* 65535 对 F767 偏大，配合取消绝对地址后 ram_heap 会占用 .bss，改为 24KB */
#undef MEM_SIZE
#define MEM_SIZE  (24 * 1024)

/* 恢复 CubeMX 生成的 TCPIP_THREAD_STACKSIZE=4096 word(16KB)。
 * 之前误按 Oil_Pump_Control 的字节值放大到 8192 word(32KB)，导致
 * FreeRTOS 堆超支。 */
#undef TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE  4096

/* RX buffer 8 字节对齐：F7 的 ETH DMA / HAL 新版 zero-copy 驱动要求 RX buffer
 * （memp RX_POOL 元素）至少 8 字节对齐，否则接收 DMA / SCB_InvalidateDCache
 * 失败导致收不到 UDP。默认 MEM_ALIGNMENT=4 只有 4 字节对齐，改为 8。
 * 参考 Oil_Pump_Control：其走 MEM_LIBC_MALLOC/MEMP_MEM_MALLOC，malloc 返回
 * 内存天然 8+ 字节对齐，故接收正常。 */
#undef MEM_ALIGNMENT
#define MEM_ALIGNMENT 8
/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /*__LWIPOPTS__H__ */
