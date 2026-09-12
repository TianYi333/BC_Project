# SensorConcentrator 项目长期笔记

## 项目概况
STM32F767ZGT6 + FreeRTOS + lwIP 传感器集中器：8 路 RS485 收传感器，以太网 TCP 上报。
- App 构建：`cmake --preset Debug` + ninja → `build/Debug/SensorConcentrator.bin`
- 工具链：arm-none-eabi-gcc @ `/d/APP/GCC-ARM/bin`；ninja @ `/c/Users/Lenovo/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin/`

## Flash / 存储
- **⚠️ F767ZGT6 = 1MB Flash（G=1MB；2MB 是 ZI 后缀）。** 双 Bank（nDBANK=0）：Bank1=512KB @0x08000000（S0~S3=16K, S4=64K, S5~S7=128K），Bank2=512KB @0x08080000（S12~S15=16K, S16=64K, S17~S19=128K）。扇区号不连续（AN4826 图1）。**Bank2 的 `FLASH_CR.SNB` 寄存器值 = 扇区号 + 4（HAL `FLASH_Erase_Sector` "+4" 规则）→ 16~23**；Bank1 直接用扇区号。
- FAL + FlashDB；KV 区 = Bank2 S12~S15 @0x08080000（64KB = 4×16KB），分区 `ef_kvdb1`，`FDB_WRITE_GRAN=64`。单 KVDB（`syncif.c` 全局 `kvdb`），存 `net_cfg`/`reboot_info`/`sensor_mode_cfg`。
- **⚠️ RAM 驻留驱动**：编程/擦除期间 Flash 停摆，CPU 不能从被操作 Flash 取指。`fal_flash_stm32f7_port.c` 的 `write/erase/ram_flash_*` 全放 `.RamFunc`（SRAM 执行，寄存器直写），startup `LoopCopyDataInit` 拷贝 → 写 Flash 不崩。**MPU region 0 必须 `DisableExec=ENABLE` 且覆盖该区（现 512KB）**，被标 XN 即 `MemManage` HardFault（`main.c` 会被 CubeMX 还原，改完 `nm` 核对 elf）。
- **⚠️ 双字编程必须拆两次独立字编程**：SRAM 执行太快，仿 HAL 双字序列偶发 **PGPERR(SR bit6)**。改为两次 32 位字编程（各 `PSIZE=WORD`+`PG`+写4B+`__DSB`+等`BSY`+清`PG`）。
- **⚠️ FAL 端口 `write()` 必须返回写入字节数**（契约见 `fal_partition.c` `@return >= 0: successful write data size`）。曾返回 0 → `fal_partition_write` 透传 → `ota.c` 的 `wr != len` 把成功误判失败，表现为 TFTP `ERROR 2: error writing file`（首包 off=0 即报）。FlashDB 只判 `< 0`，无副作用。
- **⚠️ FAL 写须 8 字节对齐**（`stm32_onchip.write_gran=64`）：OTA 末块常非 8 倍数（203276B 固件末块 12B）→ 驱动拒写。`ota_feed_chunk()` 尾部补 `0xFF` 到 8 字节后再写，**CRC 与写指针仍用真实 len**（与 boot `calc_crc(FW_A,new_len)` 一致）。
- `STM32F767xx_FLASH.ld` **零 USER CODE 区**，被 CubeMX 覆盖会复现 DTCM HardFault（须手动恢复 RAM `ORIGIN=0x20020000/LENGTH=384K`（跳过 128KB DTCM，ETH DMA 不可达）+ ETH 段 + FLASH `384K`）。症状：插网线即 HardFault。
- 烧录：现双 Bank 直接下；若全擦变单 Bank 会停 Panic，需 CubeProgrammer 设 `nDBANK=0` 再下。

## OTA 升级（内部 Flash 双 Bank，无外部 Flash）
- **架构**：独立 Bootloader @0x08000000（Bank1 S0~S4, 128KB）+ App 重定位 @0x080A0000（Bank2 S17~S19, 384KB）。Bootloader 于 2026-09-11 起为**独立顶层工程 `D:\Desktop\SensorConcentrator_boot`**（自带 CMakeLists/工具链/CMSIS/startup/boot.ld），本工程不再含 boot 子目标。
- **防砖 = 仅校验后烧录（无备份）**：1MB 恰好占满（Boot128K+fw_a384K+KV64K+meta64K+App384K），无法备份第二份 App。boot 烧录前校验 CRC32 + 向量表(SP/PC)，失败绝不跳转；App 启动后 `ota_app_confirm()` 把 DONE→CONFIRMED。
- **物理布局**：Bootloader@Bank1 S0~S4(0x08000000,128KB) / firmware_a@Bank1 S5~S7(0x08020000,384KB) / ef_kvdb1@Bank2 S12~S15(0x08080000,64KB) / ota_meta@Bank2 S16(0x08090000,64KB) / App@Bank2 S17~S19(0x080A0000,384KB)。FAL `g_sec[11]` 显式非均匀扇区表、`blk_size=16K`。
- **跨 Bank 写**：App 在 Bank2——写 firmware_a(Bank1) 跨 Bank=RWW；写 KVDB/meta(Bank2) 同 Bank（靠 .RamFunc）。Boot 在 Bank1——写 meta/App 跨 Bank。
- **状态机**：NONE=0 / PENDING=1 / DONE=2 / CONFIRMED=3 / FAILED=4；magic `OTA_META_MAGIC=0x4C55424F`。偏移见 `UserCode/Drivers/ota/ota_common.h`：`OTA_FW_A_OFFSET=0x08020000`、`OTA_FW_PART_SIZE=0x60000`、`OTA_META_OFFSET=0x08090000`、`OTA_META_SIZE=0x10000`、`OTA_APP_ADDRESS=0x080A0000`、`OTA_APP_MAX_SIZE=0x60000`。boot `burn_app` 擦 SNB={21,22,23}，`write_meta` 擦 SNB=20。
- **F7 Flash 寄存器级**（区别于 H7 的 CR2/SR2）：单 `FLASH->CR/SR/KEYR`；PSIZE[9:8]、SNB[7:3]、错误 `OPERR/WRPERR/PGAERR/PGPERR/ERSERR`、`EOP`、`BSY`；双 Bank 检测 `FLASH_OPTCR_nDBANK`(bit29)。
- **App 自确认接线**：`Core/Src/freertos.c` `StartDefaultTask()` 在 `net_config_init()` 后调 `ota_app_confirm()`（须 `#include "ota.h"`）。
- **boot `app_valid()` 坑**：App 初始 MSP=0x20080000（SRAM 顶），判定须 `sp <= 0x20080000UL`，用 `<` 会误拒合法 App → 卡死不跳。
- **TFTP 文件名 ≤20 字符**：lwIP `TFTP_MAX_FILENAME_LEN` 默认 20，超长回 ERROR 2（"SensorConcentrator.bin"=22 被拒）。现用 `ota.h:OTA_TFTP_FILENAME="SensorConc.bin"`（14），须与 `ota_tftp_push.py:TFTP_FILENAME_DEFAULT` 一致。
- **构建（两工程）**：App 本工程预设构建，须确认 `SystemInit` 写 `VTOR=0x080A0000`；Boot `D:\Desktop\SensorConcentrator_boot`：`cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake -G Ninja` + `cmake --build build`（换生成器前需删 `build/`）。其 `Core/Inc/ota_common.h` 是本工程 `ota_common.h` 的**独立快照**，改偏移须两边同步。

## RS485 采集
- 8 路 = UART4/5/7/8 + USART1/2/3/6，115200 8N1；UART7/8 无 DMA 走中断，其余走 DMA；统一 `HAL_UARTEx_ReceiveToIdle_DMA/IT`，方向脚 `*_RTS_Pin`。
- **通道映射（2026-09-11 核对）**：CH1→UART7 / CH2→UART8 / CH3→USART3 / CH4→UART5 / CH5→USART1 / CH6→USART6 / CH7→UART4 / CH8→USART2（见 `sensor_hub.c` 的 `g_ports[]`）。
- 从机地址 `0x01`，功能码 `04`（`06` 预留）；寄存器 `voltage_flag(0)`/`temp_flag(1)`/`temperature float32(2-3)`；**无电压寄存器** → `voltage` 恒 0。`SH_DEBUG_UART_ID`（默认1，0=不占）可占一路作 printf 口。

## printf 重定向坑
newlib-nano + `-ffunction-sections` + `-Wl,--gc-sections` 下真正出口是 **`_write_r()`**，不是 `syscalls.c` 里 `__weak` 的 `_write()`（会被 gc 回收）。**必须提供 `_write_r()` 强定义**（见 `debug_uart.c`）。

## 网络栈（lwIP）关键结论
- **① 连接存活检测（勿回退到应用层心跳）**：物理断线靠 `ethernetif.c` PHY 轮询 + `net_comm_task.c` `netif_is_link_up()` 边沿（断→`tcp_pending_close=1`，通→`g_fast_reconnect=1`）；链路 up 但对端死靠 **lwIP TCP Keepalive**（`LWIP_TCP_KEEPALIVE=1`，10s/3s/5 次，PCB 创建时 `ip_set_option(pcb, SOF_KEEPALIVE)` 在 `LOCK_TCPIP_CORE()` 内）→ `tcp_abort` → `errf` → 重连。应用层心跳宏已删，**勿加回**。
- **② netif API 上下文**：`LWIP_TCPIP_CORE_LOCKING=1` 时 `netif_set_*`/`dhcp_*`/`autoip_*` 首行 `LWIP_ASSERT_CORE_LOCKED()`，非 tcpip 线程一律走 `netifapi_*`（需 `network_task.h` include `lwip/netifapi.h` **且** `lwipopts.h` `#define LWIP_NETIF_API 1`）。改用 netifapi 后**必须删原 `LOCK_TCPIP_CORE/UNLOCK` 包裹**，否则死锁；tcpip_thread STKOF 先怀疑错线程调 netif API。
- **③ ARP 字节序**：`lwipopts.h` 重定义 `IPADDR_WORDALIGNED_COPY_FROM/TO_IP4_ADDR_T` 时**绝不能加 `PP_HTONS`**（原生纯 SMEMCPY，`ip4_addr_t.addr` 已是网络序，加 HTONS 会把 ARP 里的 IP 反转）→ 设备永不回 ARP + 解析不到网关 → TCP `err:-13`。判据：广播通/ping 不通/`arp -a` 无条目。改后 `objdump -d --disassemble=etharp_raw | grep strh` 确认仍是半字写。
- **④ CubeMX 重生成防护（铁律）**：生成区一律不改；在其前保护区下"预处理钩子"（宏重定向）让生成裸调用变正确版。范例：`ethernetif.c` 的 `/* USER CODE BEGIN ETH link init */` 用 4 个宏把 `netif_set_down/up/link_down/link_up` 重定向为 `netifapi_*`，函数体保持原生，文件末尾 `USER CODE BEGIN 8` `#undef`；`objdump -d --disassemble=ethernet_link_thread` 确认是 `netifapi_netif_common`。`[ETH] link DOWN/UP` 日志须放 `USER CODE BEGIN ETH link Thread core code for User BSP`。安全位：`UserCode/**`、`lwipopts.h` `USER CODE BEGIN 0`、`lwip.c` `H7_OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1`。
- **⑤ 上电时序**：`Udp_discovery_task` 启动循环与 `udp_discovery_pcb_create()` 改为 `!ip4_addr_isany() && netif_is_link_up()` 才启动/发 device_online（避免 ERR_RTE -4）；`ethernetif.c` 保护区比对 PHY `LAN8742_GetLinkState()` 与 MAC 寄存器，不一致则 `HAL_ETH_SetMACConfig` 重配（纠偏上电初期的 10M/HD 误判锁死）。
- **⑥ 拔插检测**：`Tcp_client_task` 自己做 `netif_is_link_up()` 边沿检测，`if(!link_up) continue` 必须放在 `tcp_pending_close` 处理**之后**；`heth.Init.MACAddr` 原为 CubeMX 生成的局部栈数组，已改 static（放 `USER CODE MACADDRESS`），否则"单播收不到、广播通"。
- **⑦ 崩溃类**：kvdb 未初始化即读会 HardFault → `net_config_init()` 开头补幂等 `init_sys_db()`；IWDGTask 须判 `hiwdg.Instance!=NULL` 才喂狗；HardFault 靠 `stm32f7xx_it.c` 各 Fault 句柄调 `Fault_Dump()`（轮询 UART 打栈帧 + SCB HFSR/CFSR/MMFAR/BFAR，不依赖 printf/堆/互斥）。
- **⑧ 上位机协议（2026-09-11 定稿）**：设备是 **TCP 客户端** 主动连上位机 **50010**；UDP 发现/online 走 **50000 广播**。。**⚠️ `Time verification failed!` 与 `Sequence number verification failed!` 目前只是告警**：`net_comm_task.c` 里两处的 `goto exit;` 都被注释掉了（约 1576/1582 行），命令会继续处理。设备 RTC 无网络校时可长期漂移（实测比上位机慢 10.72 天），只是打印告警、不影响下发；若哪天要真正启用拦截，须先解决 RTC 同步。所有 JSON 带 `sign` = `HMAC-SHA256(KEY, 规范化原文)` 取前 4 字节大写 hex；KEY 见 `UserCode/Logic/project_config.h`（`0123456789abcdef0123456789abcdef`）。**`ts` 在所有报文（含 sign 规范化原文）中一律为数字**（早期"混合类型"旧记已被实测打回）。规范化原文须与 `net_comm_task.c` 的 snprintf 逐字节一致，否则 `sign check failed` 丢包。已做离线网页生成器 **`tools/protocol_test_gen.html`**（纯 JS HMAC-SHA256，已用 Node crypto 对拍 7 类签名一致）。

## 待办 / 注意
- `sensorhub_write_mode()` 仅本地记录，未发 06 写帧（传感器无模式寄存器）。
- `SensorHub_Task` 栈 1024 字偏小（cJSON+HMAC+malloc），建议调到 2048~3072。
- RS485 回显测试：`SH_LOOPBACK_ECHO=1` 时非调试口原样回显；测全 8 路需换 `SH_DEBUG_UART_ID` 分次覆盖。
