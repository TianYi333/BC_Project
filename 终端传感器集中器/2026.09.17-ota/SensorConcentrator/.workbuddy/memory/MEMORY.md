# SensorConcentrator 项目长期笔记

## 项目概况
STM32F767ZGT6 + FreeRTOS + lwIP 传感器集中器：8 路 RS485 收传感器，以太网 TCP 上报。
- 构建：`cmake --preset Debug` + ninja → `build/Debug/SensorConcentrator.bin`；工具链 arm-none-eabi-gcc @ `/d/APP/GCC-ARM/bin`。
- ⚠️ 编辑工具偶发"报成功未落盘" → 改完必须 grep/脚本核验再编译。
- ⚠️ CMake 源文件是 `file(GLOB_RECURSE ...)`（无 CONFIGURE_DEPENDS）→ 增删 `UserCode/**/*.c` 后须重 `cmake --preset Debug`。
- 沙箱构建：PATH 前置 `D:\APP\GCC-ARM\bin` 与 `C:\Users\Lenovo\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin`（ninja 实际在此），`cmake --preset Debug` + `cmake --build build/Debug`。Boot 工程用 MinGW Makefiles（无 make 时改 `build/` 为 Ninja 或建 `build_ninja` 验证后删）。

## Flash / 存储
- ⚠️ F767ZGT6=1MB Flash(G=1MB)。双 Bank(nDBANK=0)：Bank1 S0~S7@0x08000000；Bank2 S12~S19@0x08080000。Bank2 SNB 寄存器值=扇区号+4→16~23。
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
- boot 调试打印：UART7 TX=PF7 / DE=PA5，115200；`boot_printf()` 不接 newlib（`boot_dump_flash_sr()` 解码 FLASH->SR）。
- ⚠️ **ota_meta 状态异常(2026-09-17，已坐实+已加固)**：`meta` 出现脏值（如 `0x04000002` 或整坨垃圾 `0x4CD563DF`）= S16(0x08090000) 残留旧版/实验期固件 meta，当前代码从不写该值；用户实测 CubeProgrammer 整片擦除后 OTA 正常 → 一次性遗留残留，非代码 bug。
  - **加固已实现(2026-09-17)**：(A) 自愈——App `ota.c::ota_app_confirm()` 遇 `magic!=OTA_META_MAGIC || state>OTA_STATE_FAILED` 调新增 `ota_meta_sanitize()`（擦 S16+写 NONE）；Boot `main_boot.c` 在"无 magic 分支(双 Bank)"与"switch default 分支"均置 NONE 并 `write_meta()`。(B) 判错日志——`ota_on_complete()` 现检查 `fal_partition_erase/write` 返回值并 `LOG_OTA` 报错。两端均编译通过(App FLASH 205236B/Boot 8040B)。

## RS485
- 8 路 UART4/5/7/8+USART1/2/3/6，115200；CH1→UART7(被 SH_DEBUG_UART_ID=1 占)…CH8→USART2。RX: PF6/PE0/PB11/PB12/PB15/PC7/PC11/PD6。
- 从机 0x01；FC04 读输入寄存器 / FC06 写保持寄存器；模式寄存器 0x0007（0=问答/1=主动上报）。无电压寄存器→voltage 恒 0。
- 模式写：`SensorHub_WriteReg()` 发 FC06 后阻塞等回显(信号量 g_wr_sync[8], 500ms)；回显尾部多 1 个 0x00 → `sh_write_resp_match()` 放宽 len 8~16 校验前 8 字节。
- 上电死通道：`sh_uart_flush_rx()` + `HAL_UART_ErrorCallback()` 自愈 + RX 引脚上拉。

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
- 待查：`Get_Unix_Second()` 疑似冻结(ts 恒 1788192001，RTC/SNTP 未工作)；SensorHub_Task 栈建议 2048~3072。
