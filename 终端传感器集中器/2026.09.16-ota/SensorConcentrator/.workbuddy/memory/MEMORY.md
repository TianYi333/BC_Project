# SensorConcentrator 项目长期笔记

## 项目概况
STM32F767ZGT6 + FreeRTOS + lwIP 传感器集中器：8 路 RS485 收传感器，以太网 TCP 上报。
- App 构建：`cmake --preset Debug` + ninja → `build/Debug/SensorConcentrator.bin`
- 工具链：arm-none-eabi-gcc @ `/d/APP/GCC-ARM/bin`；ninja @ `/c/Users/Lenovo/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin/`
- ⚠️ 编辑工具偶发"报成功未落盘"（2026-09-14 多次）→ **改完必须 grep/脚本核验再编译**。
- ⚠️ **源文件列表是 `file(GLOB_RECURSE ...)`（无 `CONFIGURE_DEPENDS`）**：新增/删除 `UserCode/**/*.c` 后必须重新 `cmake --preset Debug`，否则改动静默不生效。
- 沙箱构建命令（PATH 无 make/cmake 需自带）：`subprocess.run(['cmake','--preset','Debug'], cwd=项目根)` + `cmake --build build/Debug`，env PATH 前置 `D:\APP\GCC-ARM\bin` 与 `...\stm32cube\bundles\ninja\1.13.2+st.1\bin`。

## Flash / 存储
- **⚠️ F767ZGT6 = 1MB Flash（G=1MB；2MB 是 ZI 后缀）。** 双 Bank（nDBANK=0）：Bank1=512KB@0x08000000（S0~S3=16K, S4=64K, S5~S7=128K），Bank2=512KB@0x08080000（S12~S15=16K, S16=64K, S17~S19=128K），扇区号不连续（AN4826 图1）。**Bank2 的 `FLASH_CR.SNB` = 扇区号+4（HAL `FLASH_Erase_Sector` "+4" 规则）→16~23**；Bank1 直接用扇区号。
- FAL + FlashDB；KV 区 `ef_kvdb1` = Bank2 S12~S15 @0x08080000（64KB=4×16KB），`FDB_WRITE_GRAN=64`；单 KVDB（`syncif.c` 全局 `kvdb`），存 `net_cfg`/`reboot_info`/`sensor_mode_cfg`。
- **⚠️ RAM 驻留驱动**：编程/擦除期间 Flash 停摆，CPU 不能从被操作 Flash 取指 → `fal_flash_stm32f7_port.c` 的 `write/erase/ram_flash_*` 全放 `.RamFunc`（SRAM 执行、寄存器直写），startup `LoopCopyDataInit` 拷贝。**MPU region 0 必须 `DisableExec=ENABLE` 且覆盖该区（现 512KB）**，被标 XN 即 `MemManage` HardFault（`main.c` 会被 CubeMX 还原，改完用 `nm` 核对 elf）。
- **⚠️ 双字编程必须拆两次独立字编程**：SRAM 执行太快，仿 HAL 双字序列偶发 **PGPERR(SR bit6)**。现做两次 32 位字编程（各 `PSIZE=WORD`+`PG`+写4B+`__DSB`+等 `BSY`+清 `PG`）。
- **⚠️ FAL 端口 `write()` 必须返回写入字节数**（`fal_partition.c` 契约 `@return >=0: successful write data size`）。曾返回 0 → `ota.c` 的 `wr != len` 误判失败 → TFTP `ERROR 2: error writing file`（首包 off=0 即报）。FlashDB 只判 `<0`，无副作用。
- **⚠️ FAL 写须 8 字节对齐**（`stm32_onchip.write_gran=64`）。OTA 末块常非 8 倍数（203276B 末块 12B）→ `ota_feed_chunk()` 尾部补 `0xFF` 到 8 字节再写，**CRC 与写指针仍用真实 len**。
- `STM32F767xx_FLASH.ld` **零 USER CODE 区**，被 CubeMX 覆盖会复现 DTCM HardFault（须手动恢复 RAM `ORIGIN=0x20020000/LENGTH=384K`（跳过 128KB DTCM，ETH DMA 不可达）+ ETH 段 + FLASH `384K`）。症状：插网线即 HardFault。
- 烧录：现双 Bank 直接下；若全擦变单 Bank 会停 Panic，需 CubeProgrammer 设 `nDBANK=0` 再下。

## OTA 升级（内部 Flash 双 Bank，无外部 Flash）
- **架构**：独立 Bootloader @0x08000000（Bank1 S0~S4, 128KB）+ App 重定位 @0x080A0000（Bank2 S17~S19, 384KB）。Bootloader 自 2026-09-11 起是**独立顶层工程 `D:\Desktop\SensorConcentrator_boot`**（自带 CMakeLists/工具链/CMSIS/startup/boot.ld），本工程无 boot 子目标。
- **物理布局**：Boot@Bank1 S0~S4(0x08000000,128K) / firmware_a@Bank1 S5~S7(0x08020000,384K) / ef_kvdb1@Bank2 S12~S15(0x08080000,64K) / ota_meta@Bank2 S16(0x08090000,64K) / App@Bank2 S17~S19(0x080A0000,384K)。FAL `g_sec[11]` 显式非均匀扇区表、`blk_size=16K`。
- **防砖 = 仅校验后烧录（无备份）**：1MB 恰好占满，无法留第二份 App。boot 烧录前校验 CRC32 + 向量表(SP/PC)，失败绝不跳转；App 启动后 `ota_app_confirm()` 把 DONE→CONFIRMED。
- **跨 Bank 写**：App 在 Bank2——写 firmware_a(Bank1) 跨 Bank=RWW；写 KVDB/meta(Bank2) 同 Bank（靠 .RamFunc）。Boot 在 Bank1——写 meta/App 跨 Bank。
- **状态机**：NONE=0/PENDING=1/DONE=2/CONFIRMED=3/FAILED=4；magic `0x4C55424F`。偏移见 `ota_common.h`：`OTA_FW_A_OFFSET=0x08020000`、`OTA_FW_PART_SIZE=0x60000`、`OTA_META_OFFSET=0x08090000`、`OTA_META_SIZE=0x10000`、`OTA_APP_ADDRESS=0x080A0000`、`OTA_APP_MAX_SIZE=0x60000`。boot `burn_app` 擦 SNB={21,22,23}，`write_meta` 擦 SNB=20。
- **F7 Flash 寄存器级**（区别于 H7 的 CR2/SR2）：单 `FLASH->CR/SR/KEYR`；`PSIZE[9:8]`、`SNB[7:3]`、错误 `OPERR/WRPERR/PGAERR/PGPERR/ERSERR`、`EOP`、`BSY`；双 Bank 检测 `FLASH_OPTCR_nDBANK`(bit29)。
- **App 自确认接线**：`Core/Src/freertos.c` 的 `StartDefaultTask()` 在 `net_config_init()` 后调 `ota_app_confirm()`（须 `#include "ota.h"`）。`UserCode/Logic/main_logic.c` 的 `MainLogicTask()` 是**早期同用途的遗留任务，从未被创建**（2026-09-16 核实：`--gc-sections` 已将其丢弃，占 0 字节）。
- **`app_valid()` 坑**：App 初始 MSP=0x20080000（SRAM 顶），判定须 `sp <= 0x20080000UL`，用 `<` 会误拒合法 App → 卡死不跳。
- **TFTP 文件名 ≤20 字符**（lwIP `TFTP_MAX_FILENAME_LEN`）。现用 `ota.h:OTA_TFTP_FILENAME="SensorConc.bin"`，须与 `ota_tftp_push.py:TFTP_FILENAME_DEFAULT` 一致。
- **构建**：App 用本工程预设构建，须确认 `SystemInit` 写 `VTOR=0x080A0000`。Boot 工程**统一只用 `build/`（Makefile 生成器）**；⚠️ 沙箱 shell PATH 无 make，确需在此构建只能临时建 Ninja 目录（`cmake -B <tmpdir> -S . -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake -G Ninja`）**用完即删**。boot 的 `Core/Inc/ota_common.h` 是本工程 `ota_common.h` 的**独立快照**，改偏移须两边同步。
- **Boot 调试打印（2026-09-12）**：`boot_uart.{h,c}` —— 寄存器级 UART7 **TX=PF7(AF8)** + DE=**PA5**（与 App `SH_DEBUG_UART_ID=1` 的 CH1 同口），115200 8N1，波特率基准 **HSI 16MHz / PCLK1 不分频**（`BOOT_PCLK1_HZ`；boot 开 PLL 必须改）。自写 `boot_printf()` 不接 newlib（避 nano+gc-sections 的 `_write_r` 坑）；`boot_dump_flash_sr()` 解码 FLASH->SR 错误位；`main_boot.c` 全流程 `[BOOT]` 打印。GPIO 成员名是 **`OTYPER`** 非 `OTYPE`。固件 3768B→8008B。

## RS485 采集
- 8 路 = UART4/5/7/8 + USART1/2/3/6，115200 8N1；UART7/8 无 DMA 走中断，其余 DMA；统一 `HAL_UARTEx_ReceiveToIdle_DMA/IT`，方向脚 `*_RTS_Pin`。
- **通道映射**：CH1→UART7 / CH2→UART8 / CH3→USART3 / CH4→UART5 / CH5→USART1 / CH6→USART6 / CH7→UART4 / CH8→USART2（`sensor_hub.c` 的 `g_ports[]`）。RX 引脚：PF6/PE0/PB11/PB12/PB15/PC7/PC11/PD6。
- 从机地址 `0x01`；**FC04 读输入寄存器、FC06 写单个保持寄存器**；模式寄存器 = **0x0007**（03/06，uint16，**0=Modbus 问答 / 1=主动上报**，与 App `SENSOR_MODE_REG/ACTIVE` 直传）。寄存器 `voltage_flag(0)`/`temp_flag(1)`/`temperature float32(2-3)`，**无电压寄存器** → `voltage` 恒 0。`SH_DEBUG_UART_ID`（默认1，0=不占）占一路作 printf 口。
- **模式写（2026-09-14）**：`SensorHub_WriteReg()` 发 FC06 后阻塞等回显（信号量 `g_wr_sync[8]`，`SH_MODE_RESP_TIMEOUT_MS=500`）；应答 = 请求帧原样回显，返回 0/-3(超时=离线)/-4(回显不符)。**实测该传感器回显尾部多带 1 个 `0x00`**（9 字节）→ `sh_write_resp_match()` 已放宽为 len 8~16、只校验前 8 字节。`net_comm_task.c::sensorhub_write_mode()` 已去桩；`sensor_id=0` 全 8 路串行最坏 4s，CH1 被调试口占用必失败。
- **上电即死通道修复（2026-09-14）**：上电窗口期传感器已在发数据 → `Init` 到 `ReceiveToIdle` 之间攒下 ORE → HAL 收尾关闭接收且工程无 `HAL_UART_ErrorCallback` 强实现 → 该路永久不进中断。修法：`sh_uart_flush_rx()`（读 RDR + `RXFRQ` + 清 ICR 的 PE/FE/**NCF（注意不是 NECF）**/ORE/IDLE）在启动接收前调用；新增 `HAL_UART_ErrorCallback()` 清错后 `sh_start_rx()` 自愈；8 路 RX 引脚在 `Core/Src/usart.c` 各 `MspInit 1` 保护区改 PULLUP。

## printf 重定向坑
newlib-nano + `-ffunction-sections` + `-Wl,--gc-sections` 下真正出口是 **`_write_r()`**，不是 `syscalls.c` 里 `__weak` 的 `_write()`（会被 gc 回收）。**必须提供 `_write_r()` 强定义**（见 `debug_uart.c`）。（同条已写入用户级跨项目笔记）

## 网络栈（lwIP）关键结论
- **① 连接存活检测**：物理断线靠 `ethernetif.c` PHY 轮询 + `net_comm_task.c` 的 `netif_is_link_up()` 边沿（断→`tcp_pending_close=1`，通→`g_fast_reconnect=1`）；链路 up 但对端死靠 **lwIP TCP Keepalive**（`LWIP_TCP_KEEPALIVE=1`，`keep_idle=10s`/`intvl=3s`/`cnt=5`，PCB 创建时 `LOCK_TCPIP_CORE()` 内 `ip_set_option(pcb, SOF_KEEPALIVE)`）。应用层心跳宏已删，**勿加回**。
- **①b ⚠️ keepalive 机制（2026-09-16 订正，别再搞反）**：判据在 `tcp_slowtmr`（`tcp.c:1332`）：`(tcp_ticks - pcb->tmr) > (keep_idle + cnt*intvl)/TCP_SLOW_INTERVAL`（总时长 25s）。但 **`tcp_process()`（`core/tcp_in.c:843-847`）对每个入站报文都执行 `pcb->tmr = tcp_ticks;` 且 `pcb->keep_cnt_sent = 0;`** —— 包括**本机发数据换回来的纯 ACK**。故设备每 10s 发帧、上位机每 10s 回 ACK 时 tmr 每 10s 刷新，idle 上限 ~20 ticks 而触发要求严格 `>` 20 → **连第一次探测都到不了**。结论：**"上位机静默"不会让 keepalive 杀连接**；曾据此提出的"发送时补 `pcb->tmr = tcp_ticks`"方案**已作废，勿实现**。keepalive 只在真收不到任何 ACK（链路断/对端进程死）时才起作用——那正是它的用途。
- **② TCP 重连判定与指纹**：`Tcp_client_task` 只在 `tcp_pcb == NULL` 时建链（`LOCK_TCPIP_CORE()` 内二次防护）。**⚠️ `register` 的 seq 不作为重启判据** —— `tcp_send_register()` 里有 `g_tcp_seq = 1;` 强制重置，每次重连的 register 都是 seq=1。区分"重连 vs 重启"只能看**新 online 前 5 秒的串口行**：有 `ETH link DOWN` / `TCP error (mark async close), err:N` / `tcp_write fail, err:N` / `handshake fail` / `Server IP changed` → 重连；只有 App 启动横幅 → 重启。重连等待 = `g_fast_reconnect?(2s):(5s)` + 握手。
- **③ netif API 上下文**：`LWIP_TCPIP_CORE_LOCKING=1` 时 `netif_set_*`/`dhcp_*`/`autoip_*` 首行 `LWIP_ASSERT_CORE_LOCKED()`，非 tcpip 线程一律走 `netifapi_*`（需 include `lwip/netifapi.h` **且** `lwipopts.h` `#define LWIP_NETIF_API 1`）。改用 netifapi 后**必须删原 `LOCK_TCPIP_CORE/UNLOCK` 包裹**，否则死锁；tcpip_thread STKOF 先怀疑错线程调 netif API。
- **④ ARP 字节序**：`lwipopts.h` 重定义 `IPADDR_WORDALIGNED_COPY_FROM/TO_IP4_ADDR_T` 时**绝不能加 `PP_HTONS`**（`ip4_addr_t.addr` 已是网络序）→ 设备永不回 ARP + 解析不到网关 → TCP `err:-13`。判据：广播通/ping 不通/`arp -a` 无条目。改后用 `objdump -d --disassemble=etharp_raw | grep strh` 确认仍是半字写。
- **⑤ CubeMX 重生成防护（铁律）**：生成区一律不改；在其前保护区下"预处理钩子"（宏重定向）。范例：`ethernetif.c` 的 `/* USER CODE BEGIN ETH link init */` 用 4 个宏把 `netif_set_down/up/link_down/link_up` 重定向为 `netifapi_*`，文件末尾 `USER CODE BEGIN 8` `#undef`；`objdump -d --disassemble=ethernet_link_thread` 确认是 `netifapi_netif_common`。`[ETH] link DOWN/UP` 日志须放 `USER CODE BEGIN ETH link Thread core code for User BSP`。安全位：`UserCode/**`、`lwipopts.h` `USER CODE BEGIN 0`、`lwip.c` 的 RTOS 定义处。
- **⑥ 上电时序**：`Udp_discovery_task` 启动循环与 `udp_discovery_pcb_create()` 改为 `!ip4_addr_isany() && netif_is_link_up()` 才启动/发 device_online（避免 ERR_RTE -4）；`ethernetif.c` 保护区比对 PHY `LAN8742_GetLinkState()` 与 MAC 寄存器，不一致则 `HAL_ETH_SetMACConfig` 重配（纠偏上电初期 10M/HD 误判锁死）。
- **⑦ 拔插检测**：`Tcp_client_task` 自己做 `netif_is_link_up()` 边沿检测，`if(!link_up) continue` 必须放在 `tcp_pending_close` 处理**之后**；`heth.Init.MACAddr` 已改 static（放 `USER CODE MACADDRESS`），否则"单播收不到、广播通"。
- **⑧ 看门狗（2026-09-12）**：`iwdg.c` 现 **Prescaler=128 / Reload=4095 / Window=4095 → 16.4 s**（LSI 离散 17~47kHz，最坏约 11 s；**原 Prescaler=4 只有 0.512 s，是"开狗就无限重启"元凶**）。⚠️ `MX_IWDG_Init()` 早于 `osKernelStart()`，喂狗靠 RTOS 任务 → **绝不能在 `MX_FREERTOS_Init()` 里 `osDelay()`**（调度器未启动时 `pxCurrentTCB` 已指向 IWDGTask，delay 会把喂狗任务自己挂起；原 `osDelay(3000)` 已删）。⚠️ **软件模式（选项字节 `IWDG_SW=1`，出厂默认）下任何系统复位都会停止 IWDG**（ST：*"Once enabled, it can only be disabled by a reset"*）→ 上电与 OTA 软复位进 boot 时狗都没在跑，**boot 不需要喂狗**。只有配成硬件看门狗（`IWDG_SW=0`）才上电自动运行，那时 `main_boot.c` 必须沿途喂狗。真正盲区只剩 App 侧关中断的 Flash 操作（OTA 擦 firmware_a 3×128KB、FlashDB GC）。建议加 `__HAL_DBGMCU_FREEZE_IWDG()`（未做）。
- **⑨ 崩溃类**：kvdb 未初始化即读会 HardFault → `net_config_init()` 开头补幂等 `init_sys_db()`；IWDGTask 须判 `hiwdg.Instance!=NULL`；HardFault 靠 `stm32f7xx_it.c` 各 Fault 句柄调 `Fault_Dump()`（轮询 UART 打栈帧 + SCB HFSR/CFSR/MMFAR/BFAR，不依赖 printf/堆/互斥）。
- **⑩ 上位机协议（2026-09-11 定稿）**：设备是 **TCP 客户端** 主动连上位机 **50010**；UDP 发现/online 走 **50000 广播**。所有 JSON 带 `sign` = `HMAC-SHA256(KEY, 规范化原文)` **取前 4 字节大写 hex**（8 字符，协议约定，勿改全量）；KEY=`0123456789abcdef0123456789abcdef`（`UserCode/Logic/project_config.h`）。`ts` 一律为数字。**规范化原文的字段必须是严格字典序**：`"mode"` 是 `"model"` 的前缀 → **`mode` 必须排在 `model` 之前**（2026-09-16 修 `tcp_send_sensor_data`，此前顺序反致 sign=79166A1A 而网关期望 AC087A4B）。`device_online`/`discover_response`/`register` 走手工构造排序串，不受插入顺序影响。离线网页生成器 `tools/protocol_test_gen.html`。⚠️ `Time verification failed!` / `Sequence number verification failed!` 目前**只是告警**（两处 `goto exit;` 被注释）。

## 待办 / 注意
- **⚠️ `Get_Unix_Second()` 疑似冻结**：2026-09-16 实测 96 秒窗口内所有报文 `ts` 恒为 `1788192001`（含两次新建连接），register 的 sign 也因此完全相同。RTC 或 SNTP 未正常工作（SNTP 配的是 `g_server_ip`，上位机不开 NTP 服务）。待查 `Get_Unix_Second()` 与 RTC（LSE/LSI）状态。
- `SensorHub_Task` 栈 1024 字偏小（cJSON+HMAC+malloc），建议 2048~3072。
- RS485 回显测试：`SH_LOOPBACK_ECHO=1` 时非调试口原样回显；测全 8 路需换 `SH_DEBUG_UART_ID` 分次覆盖。
