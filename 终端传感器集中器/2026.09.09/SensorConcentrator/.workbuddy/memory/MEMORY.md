# SensorConcentrator 项目长期笔记

## 项目概况
STM32F767ZGT6 + FreeRTOS + LWIP 传感器集中器：8 路 RS485 各接 1 个终端传感器（类 Modbus 自定义协议），
以太网 TCP 上报网关。构建：`cmake --preset Debug` + ninja（build/Debug）。
工具链：arm-none-eabi-gcc @ /d/APP/GCC-ARM/bin；ninja @ /c/Users/Lenovo/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin/

## 关键技术约束

### 存储（内部 Flash）
- 已移除 SFUD / SPI W25Q64，改用 **STM32 内部 Flash**：FAL + FlashDB，
  KV 区 = **Bank2 的 S17+S18 @ 0x080A0000（256KB，2×128KB 均匀扇区）**，分区 `ef_kvdb1`。
- **单 KVDB**（`syncif.c` 的全局 `kvdb`），存 `net_cfg` / `reboot_info` / `sensor_mode_cfg`。
  单库原因：FlashDB 每库需 ≥2 扇区；用户数据约 64KB，256KB 余量充足。
- `FDB_WRITE_GRAN=64`（F767 双字编程）。
- **⚠️ 双 Bank 前提 + RAM 驻留 Flash 驱动（关键，F767 易踩坑）**：
  STM32F767 内部 Flash 编程/擦除期间**整块 Flash 阵列停摆，CPU 不能从被操作的 Flash 取指令**。
  - 布局固定**双 Bank**：代码在 Bank1、KVDB 在 Bank2（不同 Bank）。双 Bank 下 0x08080000~0x0809FFFF
    扇区不均等，**不用**；只用 Bank2 从 `0x080A0000` 起的均匀 128KB 扇区（S17/S18）。
  - `fal_flash_stm32f7_port.c` 的 `write()/erase()` 及底层 `ram_flash_*` 全部用
    `__attribute__((section(".RamFunc")))` 放 **SRAM 执行**（寄存器直写，不调 HAL/库），
    startup 的 `LoopCopyDataInit` 会把 `.RamFunc`（并入 `.data`）从 Flash 拷到 RAM。
    CPU 从 SRAM 取指，写 Flash 期间不崩——这是**终极保险**，双 Bank 与否都安全。
  - `init()` **不再自动切 Bank**（曾因改写 Option Byte 在调试器挂接时 HardFault）。改为：
    检测若当前是单 Bank(nDBANK=1) → `DebugUart_Panic` 报错 + `while(1)` 停住，提示用
    CubeProgrammer 设 nDBANK=0（双 Bank）。芯片当前已是双 Bank，正常流程不触发。
- 链接脚本 `STM32F767xx_FLASH.ld` 的 FLASH 长度收为 **512K**，强制代码只进 Bank1（双 Bank 启动区）。
  固件约 200KB，远低于 512K 上限。
- 写/擦函数内部 `PRIMASK` 关中断保护（双 Bank 下非必需但安全），恢复调用方 PRIMASK。
- **⚠️ F7 内部 Flash 双字编程在 SRAM 执行时必须拆成两次独立字编程（易踩坑，已修）**：
  `ram_flash_program_dword()` 初版仿 ST HAL 序列（清 PSIZE→设 DOUBLE_WORD→置 PG→写低32→__ISB→写高32→__DSB），
  但本驱动跑在 **SRAM(RamFunc)**，两次 32 位 store 间隔比 HAL（跑在 Flash 里）更短，F7 Flash 控制器
  来不及锁存低半字就收到高半字 → **偶发 PGPERR(bit6)** → FAL 报 `Partition write error`。仅加 ISB/DSB
  屏障不能根治（__ISB 是取指屏障，不保证第一写已到达 Flash 控制器）。最终改为：**双字 = 两次独立 32 位
  字编程**（每次 `PSIZE=FLASH_PSIZE_WORD`+`PG`+写4字节+`__DSB()`+等`BSY`+清`PG`），与 ST `FLASH_Program_Word`
  一致，彻底消除双字并行度时序依赖。SNB 17/18(Bank2 S13/S14 @0x080A0000/0x080C0000)经核对正确；PSIZE 宏
  (DOUBLE_WORD=0x300, MASK=0xFFFFFCFF)值已核对无误。
- **烧录注意**：芯片现为双 Bank，直接下载即可；若曾全擦导致单 Bank，会停在 Panice 报错，
  需先用 CubeProgrammer 把 `nDBANK=0` 设回双 Bank 再下。

### RS485 传感器采集（UserCode/Drivers/RS485）
- 8 路 = UART4/5/7/8 + USART1/2/3/6，115200 8N1；**前 6 路 DMA、后 2 路（UART7/8）中断**，
  统一 `HAL_UARTEx_ReceiveToIdle_DMA/IT`。方向脚用各路 `*_RTS_Pin`（DE/RE）。
- 从机地址统一 **0x01**；功能码 **04** 上报与查询（写用 06，预留）。
- 寄存器布局（实测，共 4 个）：`voltage_flag(0)`、`temp_flag(1)`、`temperature float32(2-3)`。
  **无电压寄存器** → 协议里 `voltage` 字段恒为 0，需另路 ADC 才可能有值。
- `SH_DEBUG_UART_ID`（默认 8，0=不占用）可临时占用一路作 printf 调试口。

### printf 重定向 —— 易踩坑
本工程用 `--specs=nano.specs` + `-ffunction-sections` + `-Wl,--gc-sections`：
printf 的真正出口是 **newlib 的 `_write_r()`**，不是 `Core/Src/syscalls.c` 里那个
`__attribute__((weak))` 的 `_write()`。后者无强引用会被 gc 回收，导致它调用的
`__io_putchar()` 一并消失 —— 表现就是串口完全无输出。
**必须提供 `_write_r()` 强定义**（见 `debug_uart.c`）。
诊断三板斧：`nm elf | grep 符号`（elf 里查不到=被回收）→ 查 map 的 `Discarded input sections`
（地址全 0）→ `objdump -d` 反汇编出口确认是否 `bl` 到自己的发送函数。

### MPU 配置 —— 易踩坑（已修复，曾因文件被还原而复现）
- **`.RamFunc` 在 SRAM 执行，MPU 绝不可把该区标成 XN**：`Core/Src/main.c` 的 `MPU_Config()` region 0
  必须 `DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE`，且 `Size` 至少覆盖 `.RamFunc` 所在 SRAM
  （现设为 `MPU_REGION_SIZE_512KB`，整片 SRAM 可执行）。一旦被标成 `MPU_INSTRUCTION_ACCESS_DISABLE`，
  跳转到 RAM 函数(`ram_flash_*`)即 `MemManage (CFSR.IACCVIOL)`，栈帧常为乱值(PC/EXC_LR 同值、xPSR 无 T 位)，
  真违例点在 `.RamFunc`(~0x20000398)。
- **坑点**：本项目 `main.c` 会被 CubeMX 重生成 / `git checkout` 还原，**改完必须重新编译并 `nm` 核对 elf 地址**，
  不能假设上次补丁还在。RAM 占用大时优先砍 LwIP 池(`PBUF_POOL_SIZE`/`ETH_RX_BUFFER_CNT`)与 FreeRTOS 堆，
  别用 XN 来"防执行"。

### LwIP netif API 调用上下文 —— 易踩坑（曾引发 tcpip_thread HardFault）
- 本工程 `LWIP_TCPIP_CORE_LOCKING=1`：`autoip_start/stop`、`netif_set_*`、`dhcp_*` 首行都带
  `LWIP_ASSERT_CORE_LOCKED()`，**必须由 `tcpip_thread` 执行（或持 core lock）**。在普通任务
  （如 `StartDefaultTask` 里的 `net_config_init()`）里直接调用 → 与 tcpip_thread 的定时器
  并发读写 netif 共享状态 → 数据竞争破坏指针/栈 → 表现为 tcpip_thread 内 STKOF(HardFault)。
  `autoip_start` 注释掉即不再崩，是判断"错线程调用"的决定性证据。
- **正确做法**：非 tcpip 线程调用一律走 **`netifapi_*`**（头注释 "To be called from non-TCPIP
  threads"，内部 marshal 到 tcpip_thread 执行）。`net_comm_task.c` 的 `net_config_init()` 现已改：
  `netifapi_netif_set_addr / netifapi_autoip_stop / netifapi_netif_set_down/up / netifapi_autoip_start`。
- **⚠️ 启用 netifapi 的两个必要条件**（缺一不可）：
  1. `net_comm_task.h` 加 `#include "lwip/netifapi.h"`；
  2. `lwipopts.h` 必须 `#define LWIP_NETIF_API 1`（默认 0！）。否则 `netifapi.h` 整份被
     `#if LWIP_NETIF_API` 条件编译屏蔽，编译器报 "implicit declaration of netifapi_*"，
     include 有了也白搭——这是比"加 include"更隐蔽的一层。
- **改用 netifapi 后必须删掉原 `LOCK_TCPIP_CORE/UNLOCK_TCPIP_CORE` 包裹**：否则持锁等待
  tcpip_thread 取同一把锁 → 死锁。netifapi 内部已在 tcpip_thread 上下文自行加锁。
- 分类判据：tcpip_thread 路径上出现 STKOF 时，**先怀疑"错线程调用 netif API 导致并发破坏"**，
  而非盲目扩 `TCPIP_THREAD_STACKSIZE`（扩栈治标不治本）。

### ✅ ARP 字节序宏 —— 曾导致"广播通、单播全不通"（最隐蔽的坑，2026-09-09 已修并实测通过）
- 为规避 UNALIGNED HardFault，本项目在 `LWIP/Target/lwipopts.h` 重定义了
  `IPADDR_WORDALIGNED_COPY_FROM/TO_IP4_ADDR_T`（拆成两个 u16 半字搬运）。
- **绝对不能在这两个宏里加 `PP_HTONS`**：原生定义（`lwip/prot/etharp.h:71/78`）是
  **纯 `SMEMCPY(dest,src,4)` 字节拷贝**。因为 `ip4_addr_t.addr` 本身已按网络字节序存储
  （小端 CPU 上内存字节就是 `C0 A8 00 78`），`addrw[0]/addrw[1]` 小端下天然拼回同序列。
- 曾多套一层 `PP_HTONS` → ARP 报文里 IP 被反转（192.168.0.120 → 168.192.120.0）：
  - `etharp.c:686` `for_us = ip4_addr_cmp(&dipaddr, netif_ip4_addr(netif))` 恒假 → **设备永不回 ARP**；
  - `etharp.c:1137` 出向 ARP 也错 → 设备自己解析不到网关/服务器 → TCP `err:-13`。
- **症状判据（牢记）**：UDP 广播通、ping 不通、单播 UDP 收不到、TCP 连不上，`arp -a` 里
  **没有设备 IP 的条目**。广播不依赖 ARP 所以能通——只要看到"广播通单播不通"，**先查 ARP**。
- 校验手法：改这类宏后 ① 写主机端 Python 脚本对照 `SMEMCPY` 验证字节；
  ② `arm-none-eabi-objdump -d --disassemble=etharp_raw | grep -E "strh|strb|str"` 确认
  仍是 `strh`（2 字节对齐半字写），没被合并成 u32 非对齐写。
- Oil_Pump_Control(H743) 的 lwipopts.h **没有**这个宏补丁，所以那边 ARP 正常——跨工程对比时
  要留意"补丁只存在于一边"。

### ⚠️ CubeMX 重生成覆盖防护（本项目高频踩坑，务必遵守）
- **铁律：CubeMX 生成区（`/* USER CODE BEGIN x */` 之外）的代码一律不改**，改了下次 Generate 必被还原。
  正确做法：在它**前面的 USER CODE 保护区**下"预处理钩子"（宏重定向 / 包装函数），
  让生成出来的裸调用在编译期自动变成正确版本。
- 已实施的范例（`LWIP/Target/ethernetif.c`）：`ethernet_link_thread` 里 CubeMX 生成的
  `netif_set_down/up/link_down/link_up` 裸调用（非 tcpip_thread 上下文，会与 tcpip_thread 并发破坏
  netif），改造方式是——在 `/* USER CODE BEGIN ETH link init */`（函数内、for 之前）定义 4 个宏
  把它们重定向为 `netifapi_netif_set_*`，函数体本身**保持 CubeMX 原生写法**。
  这样重生成多少次都安全。文件末尾 `/* USER CODE BEGIN 8 */` 里有对应 `#undef`。
  **改完必须 `objdump -d --disassemble=ethernet_link_thread` 确认产物里是
  `netifapi_netif_common` 而非裸 `netif_set_*`，不能只看源码。**
- 同理的可观测日志（如 `[ETH] link DOWN/UP`）必须放进保护区
  （现放在 `/* USER CODE BEGIN ETH link Thread core code for User BSP */`，用 `static int last_up` 边沿检测），
  否则重生成后日志静默消失、故障不可观测。
- **高危：无保护区的 CubeMX 托管文件**
  - `STM32F767xx_FLASH.ld` **整个文件零 USER CODE 区** → 一旦 Generate 即被还原。
    必须手动恢复项：`RAM ORIGIN=0x20020000 / LENGTH=384K`（跳过 128KB DTCM，ETH DMA 不可达）、
    `.RxDescripSection/.TxDescripSection/.Rx_PoolSection` 显式段、`FLASH LENGTH=512K`（双 Bank 保护）。
    被覆盖的典型症状：**插网线即 HardFault** 复现。
  - `Core/Src/main.c` 的 `MPU_Config()`：改动要放在其 USER CODE 区；经验是每次改动后
    重新编译并 `nm` 核对 elf 地址，不能假设补丁还在。
- 安全（不会被覆盖）的位置：`UserCode/**`（自建驱动）、`lwipopts.h` 的 `USER CODE BEGIN 0`、
  `LWIP/App/lwip.c` 的 `H7_OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1`。

### 网线拔插 / 链路检测（易踩坑，已修）
- **`Tcp_client_task` 必须自己感知物理链路**：只在 `tcp_pcb == NULL` 时才重连是不够的。拔线后
  PCB 仍在且 `g_tcp_connected==1`，只能等 lwIP TCP 重传超时（TCP_MAXRTX=12，指数退避，
  几十秒~几分钟）才走 `errf`。正确做法：用 `netif_is_link_up(&gnetif)`（只读 flags，无需 core lock）
  做边沿检测——断线沿置 `tcp_pending_close=1` 立即断链，上线沿置 `g_fast_reconnect=1` 跳过节流。
  注意 `if(!link_up) continue` 要放在 `tcp_pending_close` 处理**之后**，否则异步关闭被跳过。
- **`ethernet_link_thread` 不是 tcpip_thread**：其中的 `netif_set_down/up/link_down/link_up`
  一律要用 `netifapi_*`（这些函数带 `LWIP_ASSERT_CORE_LOCKED()`，netif.c:990/1028）。
- **`heth.Init.MACAddr` 悬空指针**：CubeMX 生成的 `uint8_t MACAddr[6]` 是 `low_level_init()`
  局部栈数组，函数返回后 `heth.Init.MACAddr` 即悬空。一旦二次调用 `HAL_ETH_Init()` 就会把
  栈垃圾写进 MACA0 → MAC 过滤器失效 → "单播收不到、广播通"。已改为 static 数组（放在
  USER CODE MACADDRESS 区内，CubeMX 重生成不丢）。

### 崩溃类坑（已修复，易复踩）
- **kvdb 未初始化即读取 → HardFault**：`net_config_init()` 在 `StartDefaultTask` 里于
  `GenerateDeviceSNFromUID()` 后调用 `fdb_kv_get_blob(&kvdb,"net_cfg",...)`，但 `kvdb`
  初始化（`init_sys_db()`）原在 `Task_InitTask` 内（晚于此处）。读到全零 kvdb → 解引用
  NULL。修复：在 `net_config_init()` 开头补 `init_sys_db()`（内部 `g_kvdb_inited` 幂等保护）。
  经验：**任何 `fdb_kv_*(&kvdb,...)` 调用前必须保证 `init_sys_db()` 已执行**。
- **IWDGTask 空指针喂狗 → HardFault**：`MX_IWDG_Init()` 被注释（看门狗未启用）时，
  `hiwdg.Instance` 为 NULL，而 `IWDGTask` 仍循环 `HAL_IWDG_Refresh(&hiwdg)` 解引用 NULL。
  修复：`if (hiwdg.Instance != NULL)` 才喂狗。
- **HardFault 精准定位**：`stm32f7xx_it.c` 的 Hard/Mem/Bus/UsageFault 句柄现调用
  `Fault_Dump()`，轮询 UART 打印栈帧（R0-R3,R12,EXC_LR,PC,xPSR）+ SCB 的
  HFSR/CFSR/MMFAR/BFAR（依赖 `DebugUart_Panic`/`DebugUart_PanicHex`，不依赖 printf/堆/互斥量）。
  本文件未声明 `__get_LR()`，用内联汇编 `MOV %0, LR` 读 LR。

## 待办 / 注意
- `sensorhub_write_mode()` 仅本地记录，未真正下发 06 写帧（传感器暂无模式寄存器）。
- `SensorHub_Query()` 未挂触发点（按需调用）。
- `SensorHub_Task` 栈 1024 字（4KB）偏小，其中调用 `tcp_send_sensor_data()`
  （cJSON + HMAC + malloc）有溢出风险，建议调到 2048~3072 字。
- 端口映射 `g_ports[]` 顺序按"前 6=DMA、后 2=中断"假设，实际接线需核对。
