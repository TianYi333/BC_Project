# SensorConcentrator 项目长期笔记

## 项目概况
STM32F767ZGT6 + FreeRTOS + lwIP 传感器集中器：8 路 RS485 收传感器，以太网 TCP 上报。
- 构建：`cmake --preset Debug` + ninja → `build/Debug/SensorConcentrator.bin`；工具链 arm-none-eabi-gcc @ `/d/APP/GCC-ARM/bin`。
- ⚠️ 编辑工具偶发"报成功未落盘" → 改完必须 grep/脚本核验再编译。
- ⚠️ CMake 源文件是 `file(GLOB_RECURSE ...)`（无 CONFIGURE_DEPENDS）→ 增删 `UserCode/**/*.c` 后须重 `cmake --preset Debug`。
- 沙箱构建：PATH 前置 `D:\APP\GCC-ARM\bin` 与 `C:\Users\Lenovo\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin`（ninja 实际在此），`cmake --preset Debug` + `cmake --build build/Debug`。Boot 工程用 MinGW Makefiles（无 make 时改 `build/` 为 Ninja 或建 `build_ninja` 验证后删）。

## Flash / 存储
- ⚠️ F767ZGT6=1MB Flash(G=1MB)。双 Bank(nDBANK=0)：Bank1 逻辑 S0~S7@0x08000000；Bank2 逻辑 S12~S19@0x08080000。
- ⚠️⚠️ **Bank2 的 `FLASH_CR.SNB` 寄存器值 = 逻辑扇区号 + 4 → 16~23**（Bank1 逻辑号==寄存器值）。依据本工程 HAL `stm32f7xx_hal_flash_ex.c`：**同文件有两份 `FLASH_Erase_Sector()`**，`#if defined(FLASH_OPTCR_nDBANK)`（419 行，本芯片已定义 stm32f767xx.h:7910）分支内那份（486 行）含 `if (Sector > FLASH_SECTOR_11) Sector += 4;`（511~515 行）；670 行 `#else` 那份**没有** +4，**别照着它改**。FAL 驱动 `tbl[]`、Boot `bf_erase_sector(20)`/`app_snb{21,22,23}` 用的都是寄存器值。
- 故障征兆对照：若把逻辑号 12~19 当寄存器值填 → 擦非法扇区 → `FLASH_SR=0x10`(WRPERR) → `[E/FAL] Partition erase error ... erase error!`（连打数次）、KVDB 初始化失败、设备不可用（2026-09-18 实测）。另注意 SNB=16 会命中 ef_kvdb1 首扇区，会擦掉 KV 配置。
- FAL+FlashDB；KV `ef_kvdb1`=Bank2 S12~S15@0x08080000(64KB)；`FDB_WRITE_GRAN=64`。
- ⚠️ .RamFunc：Flash 写/擦须 SRAM 执行（写期间同 Bank 取指 HardFault）；MPU region0 必须 DisableExec 且覆盖 512KB。
- ⚠️ 双字编程拆两次 32 位字编程（避 PGPERR）。
- ⚠️ FAL write() 须返回写入字节数；写须 8 字节对齐（ota_feed_chunk 末块补 0xFF，CRC/指针用真实 len）。
- ⚠️ FLASH.ld 零 USER CODE 区：RAM ORIGIN=0x20020000/LEN=384K（跳过 DTCM），否则插网线 HardFault。
- 烧录双 Bank；全擦变单 Bank 需 CubeProgrammer 设 nDBANK=0。

## OTA（内部 Flash 双 Bank，无外部 Flash）
- 独立 Bootloader `D:\Desktop\SensorConcentrator_boot` @0x08000000（S0~S4）+ App @0x080A0000（S17~S19）。boot 的 `Core/Inc/ota_common.h` 是本工程快照，改偏移须两边同步。
- 物理布局：firmware_a@0x08020000(S5~S7) / ef_kvdb1@0x08080000 / ota_meta@0x08090000(S16) / App@0x080A0000。`STM32_ONCHIP` 设备基址 0x08020000，ota_meta 分区 offset=448K→0x08090000（与 Boot `OTA_META_OFFSET` 一致）。
- 状态机 NONE0/PENDING1/DONE2/CONFIRMED3/FAILED4；magic 0x4C55424F。防砖=仅校验后烧录（无备份）。
- App 自确认：`freertos.c::StartDefaultTask` 在 `net_config_init()` 后调 `ota_app_confirm()`（DONE→CONFIRMED）。`app_valid()` 须 `sp <= 0x20080000UL`。
- TFTP 文件名 `SensorConc.bin`（≤20 字符，需与 ota_tftp_push.py 一致）。
- ⚠️ **打升级工具 exe 必须用 `D:\Desktop\OTA_Upgrade_Tool\.venv_build`（系统 Python 3.14.7 基座，带 tkinter+pyinstaller 6.22.2），勿用 WorkBuddy 托管 Python**——托管版无 tkinter，打出的 exe 启动即崩（2026-10-14 实测）。命令：`cd /d/Desktop/OTA_Upgrade_Tool && ./.venv_build/Scripts/python.exe -m PyInstaller --onefile --windowed --name OTA_Upgrade_Tool --distpath dist ota_gui.py`。升级工具已支持：等设备 `register` 报文→自动回填 device_id→再发 `upgrade`（8s 超时回退手动填写）。
- ⚠️ **App OTA 通讯协议已对齐 H743 新协议(2026-10-13，编译通过 FLASH 214676B)**：命令 `upgrade{crc(hex串),file_size,filename}` + 应答 `upgrade_ack`；新增设备主动上报 `upgrade_status`(recv_done/verify_fail/reboot/reject_fname) 与 `upgrade_result`(成功带 old_version/new_version，TCP 未连时缓存 KVDB 待连上补发)。`ota_handle_upgrade_start()` 增 `filename` 参 + 文件名校验(返回-6)；TFTP 接收按命令 filename 门禁（不再用固定 OTA_TFTP_FILENAME 宏）。工具 `D:\Desktop\OTA_Upgrade_Tool\SensorConcentrator_OTA.py` 已 `USE_LEGACY_UPGRADE_START=False` 同步。
  - **移植时保留的 SC 特有逻辑(勿照搬 H743 外部 SPI Flash 版)**：① `ota_feed_chunk` 末块 8 字节补 0xFF 对齐（内部 Flash write_gran=64bit 必需）；② `ota_handle_upgrade_start` 长度上限 `OTA_FW_PART_SIZE`(384KB，非 H743 的 512KB)；③ `ota_app_confirm` 的 `ota_meta_sanitize()` 非法 magic/state 自愈分支（H743 已删但 SC 必须留）；④ 出站签名用 SC 既有 `cjson_sort_recursive` 字典序（H743 用插入序，字段名相同故字节一致）。
- boot 调试打印：UART7 TX=PF7 / DE=PA5，115200；`boot_printf()` 不接 newlib（`boot_dump_flash_sr()` 解码 FLASH->SR）。
- ⚠️ **ota_meta 状态异常(2026-09-17，已坐实+已加固)**：`meta` 出现脏值（如 `0x04000002` 或整坨垃圾 `0x4CD563DF`）= S16(0x08090000) 残留旧版/实验期固件 meta，当前代码从不写该值；用户实测 CubeProgrammer 整片擦除后 OTA 正常 → 一次性遗留残留，非代码 bug。
  - **加固已实现(2026-09-17)**：(A) 自愈——App `ota.c::ota_app_confirm()` 遇 `magic!=OTA_META_MAGIC || state>OTA_STATE_FAILED` 调新增 `ota_meta_sanitize()`（擦 S16+写 NONE）；Boot `main_boot.c` 在"无 magic 分支(双 Bank)"与"switch default 分支"均置 NONE 并 `write_meta()`。(B) 判错日志——`ota_on_complete()` 现检查 `fal_partition_erase/write` 返回值并 `LOG_OTA` 报错。两端均编译通过(App FLASH 205236B/Boot 8040B)。

## RS485
- 8 路 UART4/5/7/8+USART1/2/3/6，115200；CH1→UART7(被 SH_DEBUG_UART_ID=1 占)…CH8→USART2。RX: PF6/PE0/PB11/PB12/PB15/PC7/PC11/PD6。
- 从机 0x01；FC04 读输入寄存器 / FC06 写保持寄存器；模式寄存器 0x0007（0=问答/1=主动上报）。无电压寄存器→sensor_data 已去除 `voltage` 字段（仅保留 `voltage_flag` 突变标志）。
- 模式写：`SensorHub_WriteReg()` 发 FC06 后阻塞等回显(信号量 g_wr_sync[8], 500ms)；回显尾部多 1 个 0x00 → `sh_write_resp_match()` 放宽 len 8~16 校验前 8 字节。
- 上电死通道：`sh_uart_flush_rx()` + `HAL_UART_ErrorCallback()` 自愈 + RX 引脚上拉。
- 上报触发（2026-09-18 重构）：8 路不再边收边报；`SensorHub_Task` 收到帧只 `sensor_cache_update()` 写 `g_sensor_cache[8]`+检测 `voltage_flag` 0→1 边沿；仅当 (a) 上位机下发 `sensor_read` 命令、(b) `voltage_flag` 由 0→1 才上报（仅变化那一路）。新增命令 type=`sensor_read`（sign 源串字典序 device_id,sensor_id,seq,ts,type），网关需配合构造。

## printf 重定向
newlib-nano+gc-sections 出口是 `_write_r()`（非 `__weak _write()`）；须提供 `_write_r()` 强定义（debug_uart.c）。

## 网络栈 lwIP
- 连接存活：物理断→`netif_is_link_up()` 边沿；对端死→TCP Keepalive(keep_idle10s/intvl3s/cnt5)。
- ⚠️ keepalive 订正：入站(含纯ACK)刷新 `pcb->tmr` → "上位机静默"不杀连接；曾提"发时补 tmr"方案已作废。
- TCP 重连：`tcp_pcb==NULL` 才建链；register seq 恒 1 不作重启判据；看 online 前 5s 串口行区分重连/重启。
- netif API：非 tcpip 线程走 `netifapi_*`（须 LWIP_NETIF_API=1），且删 LOCK_TCPIP_CORE 包裹。
- ARP：重定义字节序宏禁加 PP_HTONS。CubeMX 重生成须用 USER CODE 保护区宏钩子。
- IWDG：Prescaler=128→16.4s；软件模式复位停狗→boot 不用喂；MX_FREERTOS_Init 禁 osDelay。
- 协议：TCP 客户端连 50010，UDP 发现 50000；sign=HMAC-SHA256 取前 4 字节大写 hex；字段严格字典序(mode 在 model 前)。
- 心跳（2026-09-18 重引入，参考 Oil_Pump_Control）：上行 `heartbeat`（30s 周期，源串字典序 device_id<sensor_online<seq<status<ts<type；`sensor_online[8]` 判据与告警一致 alarm==2→0）、下行 `heartbeat_ack`（源串 device_id<seq<server_time<ts<type；server_time 仅验签不校时）。连续 `HB_LOST_MAX=3` 周期无任何合法下行→断链重连（清零点在 tcp_parse_cmd 验签通过处）；与 netif link 边沿 + TCP Keepalive 两层互补。`status` 恒 0 占位。
- ⚠️ **RTC 时间基准错误（根因已定位+纠正 2026-10-09）**：用户实测 MS_TS 单调递增（16:00:11→16:09:50→16:15:05）→ **计数器在走、LSI 正常**，上一轮"计数器冻结"误判已撤回。**真因**：
  (1) `MX_RTC_Init`(rtc.c:58-81) 的 `Check_RTC_BKUP` 用户块为空=**无备份域门控**，每次上电无条件把 RTC 改写为占位初值 `2026-09-01 00:00:00`（北京→UTC 即打印的 `2026-08-31 16:00:xx`）；整片擦除重下清掉备份域→那次"跳变一次"就是初值重新落盘。
  (2) NTP 当前 `NTP sync fail, ts=0`→`TimeSyncTask`(net_comm_task.c:2712) 每轮 `sys_unix_ms=0` 清零且刷新失败→`if(sys_unix_ms>0)` 守护的 `UnixMs_To_RTC`(:2752) 永不进→RTC 从不写真实时间→停在"初值+本次上电秒数"。
  (3) 因此"换 LSI 无效"反证卡点**不是振荡器**，而是时间基准锁死在初值+NTP 连不上。
  - **次级 bug**：`UnixMs_To_RTC`(rtc_cluck.c:29) 行66 直接写只读亚秒寄存器 `RTC->SSR`（F7 SSR 只读，直写无效）→ NTP 同步后日期/时分秒对、亚秒错（协议 ts 用秒级影响小）。
  - **当前 RTC 时钟源 = 外部 LSE（2026-10-09 后由用户改回）**：`main.c:168,170`=`RCC_OSCILLATORTYPE_LSE`+`LSEState=RCC_LSE_ON`（开外部 32.768kHz 晶振）；`rtc.c:107`=`RCC_RTCCLKSOURCE_LSE`（RTC 选 LSE）。即 RTC 已脱离片内 LSI RC，改用外部石英晶振（精度远高于 LSI）。此前诊断里"换 LSI 无效"是 LSI 时期的结论，现不再适用；若 LSE 上电后时间仍异常，先读 `RCC->BDCR` 的 `LSERDY` 确认外部晶振是否真正起振。
  - **已修复（2026-10-09，参照 H743 工程 Oil_Pump_Control 同款写法，编译通过 FLASH 211692B）**：
    ① `MX_RTC_Init` 改用硬件标志 `RTC_ISR_INITS`(rtc.c:60) 门控——`INITS=1`(VBAT 维持/日历已配) 则 `EnableBypassShadow`+`return` 跳过 `SetTime/SetDate`，不再每次上电重写初值；首次(INITS=0)初始化后 `EnableBypassShadow`(rtc.c:89)。替代原 BKUPRead 方案。
    ② `UnixMs_To_RTC`(rtc_cluck.c:55) 删掉直写只读 `RTC->SSR`+`EnableBypassShadow` 段，亚秒改由 HAL `sTime.SubSeconds/SecondFraction` 随 `SetTime` 装载(避免写脏 SSR 致读跳变)。
    ③ `RTC_To_UnixMs`/`Time_To_Unix` 的 `struct tm` 零初始化并显式 `tm_isdst=0`(rtc_cluck.c:86/146)，消除 `mktime` 因 `tm_isdst` 垃圾值误加 1 小时造成的间歇性跳变。
    ④ NTP 仍需可达 `192.168.0.10` 才能把真实时间写回 RTC（TimeSyncTask 逻辑未动）。
  - SensorHub_Task 栈建议 2048~3072。

## SensorConcentrator_Test 测试工程（D:\Desktop\SensorConcentrator_Test）
- 精简硬件测试工程：8 路 RS485 自环 + 以太网上电 UDP 周期上报（静态 IP 192.168.1.100→上位机 192.168.1.200:50000，1s/次，JSON 含 8 路回环结果+seq）。
- ⚠️ **本工程 lwIP 包仅 raw API**：`Middlewares/.../LwIP/src/include/lwip/` 下 **无 netconn.h**（sequential/socket API 未启用），UDP 发送只能用 `udp_new()`+`udp_sendto()` 并套 `LOCK_TCPIP_CORE()`/`UNLOCK_TCPIP_CORE()`（LWIP_TCPIP_CORE_LOCKING=1 已开），且 `gnetif` 需自行 `extern struct netif gnetif;`（lwip.h 未声明）。
- 构建：`cmake --preset Debug` + `cmake --build build/Debug`（ninja，PATH 前置 `/d/APP/GCC-ARM/bin` 与 ninja 目录）；产出 `build/Debug/SensorConcentrator_Test.bin`。测试固件不用 FAL，无 nDBANK 要求。
