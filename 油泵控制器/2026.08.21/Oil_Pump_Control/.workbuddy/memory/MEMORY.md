# 项目记忆：Oil_Pump_Control

## 项目定位
- 工业**润滑泵（注油泵）控制器**固件，设备型号 `LUB-PUMP-V1.0`，FW `1.0.3`，HW `V4.5`。
- MCU：**STM32H743ZGT6**（LQFP144，Cortex-M7 @480MHz），STM32Cube FW_H7 V1.12.1，CubeMX 6.15.0 生成。
- 构建：CMake + Ninja + gcc-arm-none-eabi（toolchain 在 cmake/gcc-arm-none-eabi.cmake），Debug/Release 两套 preset，编译产出 ELF/HEX/BIN。
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
