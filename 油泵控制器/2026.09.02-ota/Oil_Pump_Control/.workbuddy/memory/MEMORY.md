# 项目记忆：Oil_Pump_Control

## 项目定位
- 工业**润滑泵（注油泵）控制器**固件，设备型号 `LUB-PUMP-V1.0`，FW `1.0.3`，HW `V4.5`。
- MCU：**STM32H743ZGT6**（LQFP144，Cortex-M7 @480MHz），STM32Cube FW_H7 V1.12.1，CubeMX 6.15.0 生成。
- 构建：CMake + Ninja + gcc-arm-none-eabi（toolchain 在 cmake/gcc-arm-none-eabi.cmake），Debug/Release 两套 preset，编译产出 ELF/HEX/BIN；App 固件 bin = `build/Debug/Oil_Pump_Control.bin`（烧录地址 0x08100000），boot 为独立工程 `D:/Desktop/boot`（bin = `build/boot.bin`，烧录地址 0x08000000）。
- 工程由 STM32CubeMX 生成，用户代码入口区在 `/* USER CODE BEGIN/END */`，主逻辑全在 `UserCode/`。

## 软件架构
- RTOS：FreeRTOS CMSIS_V2，堆 200KB，开启 tickless idle、FPU、newlib reentrant。
- 主状态机（`UserCode/Logic/main_logic.c` 的 `MainLogicTask`）：
  STOP 停机 / PRESS 加压(PID调速) / HOLD 保压 / RELEASE 泄压 / SETTING 设置 / SELFTEST 自检 / FAULT 故障锁定。
- 故障位图 `sys_fault_bit` 集中在主逻辑汇总；油温过温、液位过低为**锁定故障需手动复位**，严重超压为二级可自动恢复保护。

## 任务清单（freertos.c）
- IWDGTask（喂狗200ms）、StartDefaultTask（LWIP+初始化总入口）
- MainLogicTask、PressureControlTask（PID闭环，优先级最高）、ModbusMasterTask
- ADCTask、Step_speedTask（步进电机调速，MOTOR_DRIVER_PULSE_STEPPER 时启用）
- Udp_discover_task、Tcp_client_task、TcpSenderTask、TimeSyncTask、RebootTask
- OLED 任务、Buzz 任务、Syncif_task（数据同步）、MBif_task（Modbus从站接口）
- 互斥锁：uart/flash_kv/udp_pcb/tcp_send/sys_status（递归锁）

## 驱动与外设（UserCode/Drivers）
- Sensor/ADC_collect（ADC3 4通道，BDMA循环，压力/油温/液位/备用）
- PID/pid_ctrl（压力闭环调速，含 PressureControlTask）
- Motor/step_motor（脉冲步进；可切换 RS485 伺服 MOTOR_DRIVER_RS485_SERVO）
- Modbus_master（读电机驱动器 1001/6041/6061/6060/6081 等 CiA402 寄存器）
- Sht40（I2C 温湿度）、Encoder（EC11 旋钮）、Oled（U8G2 显示）、Buzzer、Clock/rtc_clock、System/system_service
- ETH/net_comm_task + HMAC-SHA256（UDP 发现 / TCP 9530 客户端 / 时间同步）

## 第三方库（UserCode/Third_Party）
- U8G2（OLED）、FreeModbus（从站）、Fal+FlashDB+SFUD（SPI Flash 持久化参数/报警/重启原因）
- LetterShell（命令行 shell）、cJSON（JSON 协议）、Mongoose（HTTP/Web，当前可能未启用）

## 约定与注意
- 电机驱动二选一宏在 `UserCode/Logic/project_config.h`（当前 `MOTOR_DRIVER_PULSE_STEPPER`）。
- Modbus 浮点缩放宏：`FLOAT_TO_REG(f)=f*100`，`REG_TO_FLOAT(r)=r/100`，`PACK32/UNPACK32`。
- 日志宏：`LOG`/`LOG_logic`/`LOG_ADC`/`LOG_NET`/`LOG_Debug` 在 project_config.h，已开启。
- 全局状态 `_SYS_STATUS main_sys_status` 与配置 `SYS_CONFIG_T sys_cfg` 通过 `SysStatus_Read/WriteSnapshot`（带 sys_status_mutex 互斥）访问。
- **【安全】网络重放防护是故意关闭的**：`net_comm_task.c` 中 udp/tcp 的 time/seq 校验 `goto`（L773/779、L935/942、L1956/1962）被注释掉，是开发者有意为之（非缺陷），勿当作 bug 修复。签名校验仍在，仅放宽了时序/序列号校验。

## OTA 升级（App 端）
- 固件接收在 `UserCode/Drivers/ota/ota.c`，**仅保留 lwIP TFTP server（UDP/69）一种传输方式**；原 TCP 升级路径（`OTA_TRANSPORT_TCP`/`OTA_TCP_PORT`、`ota_tcp_*` 函数、`lwip/tcp.h`、`ota_tcp_push.py`）已全部移除（2026-09-02）。
- TFTP 接收文件名由 `ota.h` 宏 `OTA_TFTP_FILENAME` 定义，设备端**当前即 `"Oil_Pump_Control.bin"`**（与 App 构建产物 `build/Debug/Oil_Pump_Control.bin` 同名）；PC 端 `ota_tftp_push.py` 的 `bin` 参数默认指向该路径、`--tftp-filename` 默认同为 `Oil_Pump_Control.bin`，双端已一致。改文件名时须同步改 ota.h 宏与脚本两侧。
- 共用核心：`ota_feed_chunk`（fal 写 firmware_a + `fdb_calc_crc32` 增量）/ `ota_on_complete`（len&crc 校验→写 `ota_meta(PENDING)`→软复位）/ `ota_reset_session`（回 IDLE 允许重试）；全部 `fal_partition_*` 用 `flash_kv_mutex` 串行化。
- 触发入口 `ota_handle_upgrade_start(new_len,new_crc32)`（由 `upgrade_start` 命令调用）；启动确认 `ota_app_confirm()`（MainLogicTask 首轮循环后）。boot 端见 PENDING 烧录内部 Flash 并置 DONE，App 启动后 CONFIRMED（防砖确认点）。
- CRC：FlashDB `fdb_calc_crc32`（标准 CRC32，init=0），与 Python `zlib.crc32` 一致；PC 推送脚本为 `ota_tftp_push.py`（原 TCP 版 `ota_tcp_push.py` 已删除）。
- 另提供**图形化升级工具**：源码 `ota_gui.py`（Tkinter），打包产物 `dist/Oil_Pump_OTA_Tool.exe`（约 12MB 单文件，双击运行，无需 Python 环境）。界面可填 Device ID、选固件、改 HMAC key/TFTP 文件名，实时显示升级日志。打包环境为工程目录下 `.venv_build/`（系统 Python 3.14.7 + PyInstaller），后续改界面后可用 `.venv_build/Scripts/pyinstaller.exe --onefile --windowed --name Oil_Pump_OTA_Tool ota_gui.py` 重新生成。

## 用户协作习惯
- **改动前必须先确认**：对项目代码/配置/文档做任何修改（含新增/删除/编辑文件、编译构建等会改变项目状态的操作）前，必须先向用户给出方案、获批后再动手。只读分析、grep/搜索、内存与日志写入、用户已明确授权或明确要求直接执行的任务除外。该约定同时记于跨项目 `~/.workbuddy/MEMORY.md`。
