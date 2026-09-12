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
