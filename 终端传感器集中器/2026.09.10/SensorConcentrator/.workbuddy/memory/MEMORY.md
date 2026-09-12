# SensorConcentrator 项目长期笔记

## 项目概况
STM32F767ZGT6 + FreeRTOS + LWIP 传感器集中器：8 路 RS485（类 Modbus 自定义协议）收终端传感器，以太网 TCP 上报。
- 构建：`cmake --preset Debug` + ninja（`build/Debug`）。
- 工具链：arm-none-eabi-gcc @ `/d/APP/GCC-ARM/bin`；ninja @ `/c/Users/Lenovo/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin/`

## 存储（内部 Flash）
- 已用 **STM32 内部 Flash**：FAL + FlashDB；KV 区 = Bank2 S17+S18 @ `0x080A0000`（256KB，2×128KB），分区 `ef_kvdb1`。
- 单 KVDB（`syncif.c` 全局 `kvdb`），存 `net_cfg`/`reboot_info`/`sensor_mode_cfg`。`FDB_WRITE_GRAN=64`。
- **⚠️ 双 Bank + RAM 驻留 Flash 驱动（F767 关键坑）**：Flash 编程/擦除期间整片停摆，CPU 不能从被操作 Flash 取指。
  `fal_flash_stm32f7_port.c` 的 `write/erase/ram_flash_*` 全用 `__attribute__((section(".RamFunc")))` 放 SRAM 执行（寄存器直写），startup 的 `LoopCopyDataInit` 把 `.RamFunc` 拷进 RAM → 写 Flash 不崩（终极保险）。
- **⚠️ 双字编程必须拆两次独立字编程**：SRAM 执行太快，仿 HAL 双字序列偶发 **PGPERR(bit6)** → FAL `Partition write error`。改为两次 32 位字编程（各 `PSIZE=WORD`+`PG`+写4B+`__DSB`+等`BSY`+清`PG`）。
- 链接脚本 FLASH 长度收 **512K**（代码只进 Bank1）。`STM32F767xx_FLASH.ld` **零 USER CODE 区**，被 CubeMX 覆盖会复现 DTCM HardFault。
- 烧录：芯片现双 Bank 直接下；若全擦变单 Bank，会停 Panic，需 CubeProgrammer 设 `nDBANK=0` 再下。

## RS485 采集（UserCode/Drivers/RS485）
- 8 路 = UART4/5/7/8 + USART1/2/3/6，115200 8N1；**前 6 路 DMA、后 2 路(UART7/8) 中断**，统一 `HAL_UARTEx_ReceiveToIdle_DMA/IT`，方向脚各路 `*_RTS_Pin`。
- 从机地址 `0x01`，功能码 `04`（写 `06` 预留）。寄存器：`voltage_flag(0)`/`temp_flag(1)`/`temperature float32(2-3)`；**无电压寄存器** → `voltage` 恒 0。
- `SH_DEBUG_UART_ID`（默认 8，0=不占）可临时占一路作 printf 口。

## printf 重定向坑
newlib-nano（`--specs=nano.specs` + `-ffunction-sections` + `-Wl,--gc-sections`）：真正出口是 **`_write_r()`**，不是 `syscalls.c` 里 `__weak` 的 `_write()`（会被 gc 回收）。**必须提供 `_write_r()` 强定义**（见 `debug_uart.c`）。

## MPU 配置坑
`.RamFunc` 在 SRAM 执行 → MPU region 0 必须 `DisableExec = ENABLE` 且 Size 覆盖该区（现 `MPU_REGION_SIZE_512KB`）。被标 XN → 跳 RAM 函数即 `MemManage` HardFault。`main.c` 会被 CubeMX 还原，改完必 `nm` 核对 elf。

## 网络栈（lwIP）关键结论

### ① 连接存活检测架构（2026-09-09 定稿，勿回退到应用层心跳）
- 物理断线 → `ethernetif.c` 链路线程 PHY 轮询 + `net_comm_task.c` `netif_is_link_up()` 边沿检测（断线沿置 `tcp_pending_close=1`，上线沿置 `g_fast_reconnect=1` 跳节流），~100ms 级。
- 链路 up 但对端死 → **lwIP TCP Keepalive**（`LWIP_TCP_KEEPALIVE=1`；`KEEPIDLE=10s`/`KEEPINTVL=3s`/`KEEPCNT=5`，总 25s）。PCB 创建时 `ip_set_option(pcb, SOF_KEEPALIVE)`（在 `LOCK_TCPIP_CORE()` 内）。超时 → `tcp_abort` → `errf` → `tcp_pending_close=1` → 自动重连。
- **应用层心跳已彻底移除**（宏已删）。历史教训：原心跳只剩"累计 2 次强制断链"、清零点仅"收到下行报文" → 上位机静默 40s 设备自伤重连。**勿加回。**

### ② netif API 调用上下文坑（曾致 tcpip_thread HardFault）
- `LWIP_TCPIP_CORE_LOCKING=1`：`netif_set_*`/`dhcp_*`/`autoip_*` 首行带 `LWIP_ASSERT_CORE_LOCKED()`，必须由 `tcpip_thread` 执行。**非 tcpip 线程一律走 `netifapi_*`**。
- 启用 netifapi 两条件缺一不可：① `net_comm_task.h` `#include "lwip/netifapi.h"`；② `lwipopts.h` `#define LWIP_NETIF_API 1`（默认 0，否则整份被条件编译屏蔽）。
- 改用 netifapi 后**必须删原 `LOCK_TCPIP_CORE/UNLOCK` 包裹**，否则死锁。tcpip_thread 出现 STKOF 先怀疑"错线程调 netif API"，别盲目扩栈。

### ③ ARP 字节序宏 —— 曾致"广播通单播全不通"（最隐蔽坑，已修实测通过）
- `LWIP/Target/lwipopts.h` 重定义 `IPADDR_WORDALIGNED_COPY_FROM/TO_IP4_ADDR_T` 时**绝不能加 `PP_HTONS`**：原生是纯 `SMEMCPY`，`ip4_addr_t.addr` 已网络字节序，加 HTONS 会把 ARP 里 IP 反转（192.168.0.120 → 168.192.120.0）→ 设备永不回 ARP + 自己解析不到网关 → TCP `err:-13`。
- **判据**：UDP 广播通、ping 不通、单播收不到、TCP 连不上，`arp -a` 无设备条目 → 先查 ARP。改后 `objdump -d --disassemble=etharp_raw | grep strh` 确认仍半字写。Oil_Pump_Control(H743) 无此补丁，跨工程对比留意。

### ④ CubeMX 重生成覆盖防护（铁律）
- CubeMX 生成区（`USER CODE BEGIN/END` 之外）一律不改。正确做法：在前面的保护区下"预处理钩子"（宏重定向/包装），让生成裸调用编译期变正确版。
- 范例 `ethernetif.c`：`ethernet_link_thread` 内 `/* USER CODE BEGIN ETH link init */`（for 前）定义 4 个宏把 `netif_set_down/up/link_down/link_up` 重定向为 `netifapi_*`，函数体保持 CubeMX 原生；文件末尾 `USER CODE BEGIN 8` `#undef`。改完 `objdump -d --disassemble=ethernet_link_thread` 确认是 `netifapi_netif_common`。
- `[ETH] link DOWN/UP` 日志放 `USER CODE BEGIN ETH link Thread core code for User BSP`（`static int last_up` 边沿），否则重生成后静默。
- `STM32F767xx_FLASH.ld` 零保护区：手动恢复 RAM `ORIGIN=0x20020000/LENGTH=384K`（跳 128KB DTCM，ETH DMA 不可达）+ ETH 段 + FLASH `512K`；被覆盖典型症状插网线即 HardFault。
- 安全位：`UserCode/**`、`lwipopts.h` `USER CODE BEGIN 0`、`lwip.c` `H7_OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1`。

### ⑤ 上电时序修复（2026-09-09 末次，已编译验证）
- ① `net_comm_task.c` `Udp_discovery_task` 启动循环 + `udp_discovery_pcb_create()`（含重建路径）改为 `!ip4_addr_isany() && netif_is_link_up()` 才启动/发 device_online；发前再确认一次 link，避免 ERR_RTE(-4)。
- ② `ethernetif.c` 保护区持续比对 PHY `LAN8742_GetLinkState()` 与 MAC 寄存器（`HAL_ETH_GetMACConfig`），不一致 `HAL_ETH_SetMACConfig` 重配，纠偏上电初期 PHY 协商未完成误判 10M/HD 导致 ERR_RST 的锁死。

### ⑥ 网线拔插 / 链路检测坑（已修）
- `Tcp_client_task` 须自感物理链路：`netif_is_link_up()` 边沿检测；`if(!link_up) continue` 要放在 `tcp_pending_close` 处理**之后**。
- `heth.Init.MACAddr` 悬空指针：CubeMX 生成 `uint8_t MACAddr[6]` 局部栈数组，已改 static（放 `USER CODE MACADDRESS` 区）→ 防"单播收不到、广播通"。

### ⑦ 崩溃类坑（已修）
- kvdb 未初始化即读 → HardFault：`net_config_init()` 开头补 `init_sys_db()`（幂等），任何 `fdb_kv_*(&kvdb)` 前必保证已 init。
- IWDGTask 空指针喂狗：看门狗未启时 `hiwdg.Instance==NULL`，加 `if(hiwdg.Instance!=NULL)` 才喂。
- HardFault 定位：`stm32f7xx_it.c` 各 Fault 句柄调 `Fault_Dump()`（轮询 UART 打栈帧 + SCB HFSR/CFSR/MMFAR/BFAR，不依赖 printf/堆/互斥）。

## 待办 / 注意
- `sensorhub_write_mode()` 仅本地记录，未发 06 写帧（传感器暂无模式寄存器）。
- `SensorHub_Task` 栈 1024 字偏小（含 cJSON+HMAC+malloc），建议调到 2048~3072 字。
- `g_ports[]` 顺序按"前 6=DMA、后 2=中断"假设，实际接线需核对。
