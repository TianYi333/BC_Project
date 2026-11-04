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

## OTA 升级（App 端）—— 2026-10-12 已对齐 H743_VS_GCC_IDE 新协议
- 固件接收在 `UserCode/Drivers/ota/ota.c`，**仅保留 lwIP TFTP server（UDP/69）一种传输方式**；原 TCP 升级路径已全部移除（2026-09-02）。
- **命令协议（新，与 H743_VS_GCC_IDE 完全一致）**：网关下发 `upgrade{crc:"8位大写HEX", file_size, filename}` → 设备应答 `upgrade_ack{status,message,estimated_duration}`（`status=0` 接受）。
  - 签名源字面量：`{"crc":"%s","device_id":"%s","file_size":%lu,"filename":"%s","seq":%lu,"ts":%s,"type":"upgrade"}`（crc 用原始字符串、ts 用 ts_cmd_str）。
  - `ota_handle_upgrade_start(new_len, new_crc32, filename)`；filename 长度须 1..`OTA_FILENAME_MAX_LEN`(20)，非法返回 `-6`；ack 的 status = `-ota_ret`。
  - **TFTP 接收文件名不再由宏决定**，改由本次命令下发的 filename 决定（存在 `g_ota_exp_fname`，`tftp_open_cb` 严格比对）。`ota.h` 的 `OTA_TFTP_FILENAME`（本工程 `"Oil_Pump_Control.bin"`）现仅作网关侧默认参考。
- 过程上报 `upgrade_status{status,message}`：`recv_done`（收满）/ `verify_fail`（校验失败）/ `reboot`（校验通过即将复位）/ **`reject_fname`（TFTP 文件名门禁失败，2026-10-13 补）**。
  - `reject_fname` 位于 `tftp_open_cb()` 文件名不符分支：**只上报、绝不清理会话**——该回调在 lwIP 收包上下文，调 `tftp_cleanup()`/`ota_reset_session()` 会释放正在使用的 upcb 后仍要发 ERROR 包 → 崩溃；TFTP 保持监听以便网关用正确文件名重推。
- 结果上报 `upgrade_result{result,message,model,new_version,old_version}`：由 `ota_app_confirm()` 确认成功后调 `tcp_report_upgrade_result(1,"upgrade_success")`；TCP 未连上则缓存 FlashDB（KV `ota_result`，magic `0x4F544152` "OTAR"），连接成功后由 `upgrade_result_check_and_report()` 补发并删除。升级前版本由 `tcp_store_ota_old_version()` 存 KV `ota_oldver`。
- 共用核心：`ota_feed_chunk`（fal 写 firmware_a + `fdb_calc_crc32` 增量）/ `ota_on_complete`（len&crc 校验→写 `ota_meta(PENDING)`→软复位）/ `ota_reset_session`；全部 `fal_partition_*` 用 `flash_kv_mutex` 串行化。启动确认 `ota_app_confirm()`（MainLogicTask 首轮循环后）。boot 见 PENDING 烧录内部 Flash 并置 DONE，App 启动 CONFIRMED（防砖点）。
- CRC：FlashDB `fdb_calc_crc32`（标准 CRC32，init=0），与 Python `zlib.crc32` 一致。
- App 固件上限：`OTA_APP_MAX_SIZE = 0x00080000`（512KB，H743ZGT6 bank2）。注意 `D:\Desktop\boot\Core\Inc\ota_common.h` 里写的是 `0x00100000`（1MB，注释"bank2 共 1MB"），与两个 App 的 512KB 不一致——boot 只用于长度上限判断，暂未影响，改动 boot 时留意。
- PC 侧工具目录 `D:\Desktop\OTA_Upgrade_Tool`：
  - `ota_tftp_push.py`（CLI）：新协议；`TFTP_FILENAME_DEFAULT="Oil_Pump_Control.bin"`；长度上限常量 `OTA_APP_MAX_SIZE=0x80000`；另含旧协议兼容层 `send_upgrade_start()` / `build_sign_src_legacy()` / `LEGACY_ACK_KEYWORD`。
  - `ota_gui.py`（GUI 基座，多工程复用）：类属性定制 `DEFAULT_BIN` / `DEFAULT_TFTP_FILENAME` / `USE_LEGACY_UPGRADE_START`（False=新协议 upgrade；True=旧协议 upgrade_start）。
  - `SensorConcentrator_OTA.py`：置 `USE_LEGACY_UPGRADE_START=True`、`DEFAULT_TFTP_FILENAME="SensorConc.bin"`（其 F767 固件仍是旧协议）。
  - 打包产物：`dist/OTA_Upgrade_Tool.exe`（本工程，新协议）、`dist/SensorConcentrator_OTA_Tool.exe`。打包用 `.venv_build/`（系统 Python 3.14.7 + PyInstaller 6.22.2，含 tkinter）。


## 与参照工程 H743_VS_GCC_IDE 的同步约定
- 两工程**均非 git 仓库**，且同名文件常是不同产品内容（如 `oled_page.c`：参照 ~1263 行 / 本工程 ~3548 行），**禁止整文件覆盖**，只能按改动点定点移植。
- 定位"参照工程本次改了什么"的可靠基线：`D:\Desktop\temporary\<日期-版本>\H743_VS_GCC_IDE` 快照目录（取 mtime 较旧的那个与 live 做 `diff -u`）。注意有的快照目录内容其实是**改动后**的当前状态（mtime 与 live 相同），不能当基线。
- 定点核对手法：Python `difflib` 按「函数定义 + `\n{`」到「下一个函数定义」切片比对；起止标记若撞到原型声明会横跨全文件产生海量噪声。
- 已同步批次：①2026-10-12 OTA 新协议（ota 三文件整覆盖 + net_comm_task.c 命令/上报）；②2026-10-12 二次改动（net_comm_task.c 加 `g_ota_result_pending` 惰性补发 —— **已保留**；oled_page.c 的 About Device 改 DEVICE_FW_VER —— **本工程不适用已回退**，因本工程无 About Device 菜单入口、`about_device_callback()` 是死代码，版本已在 System Info→Firmware Ver 显示）；③2026-10-13 `ota.c` 的 `tftp_open_cb` 加 `reject_fname` 上报（同步后**两工程 ota.c 全文件 IDENTICAL**）。
- **同步铁律**：移植参照工程改动前，先确认目标锚点在本工程**是否可达**（同名函数可能在本工程未挂进菜单/是死代码），否则改动会落到永不执行的位置。

## JSON 报文签名规则（net_comm_task.c，已逐一核对，2026-09-16）
- HMAC 密钥默认 `project_config.h` 的 `HMAC_KEY = "0123456789abcdef0123456789abcdef"`。
- 签名算法：对「签名源串」做 HMAC-SHA256，取**前 4 字节大写 hex（8 字符）**作为 `sign`。
- **签名源串统一为「全字段字典序紧凑 JSON（不含 sign）」**（cJSON_PrintUnformatted of temp_root，temp_root 严格字典序添加 key）。**唯一例外**：`config_set_resp` 的签名源 = `{device_id, seq, ts, type}` 四字段字典序，**不含 `device_config`**（完整响应里含 device_config，校验时要剔除）。
- 易错点（已踩坑）：`state_response` 模板顺序 `dev_status,device_id,...` 看似非字典序，实则 `_`(0x5F)<`i`(0x69)，`dev_status`<`device_id`，仍是字典序；`heartbeat` 的「最终发送 JSON」是 type 在前，但**签名源 temp_root 是字典序**，校验须按字典序重建。
- 通用校验策略：候选1 全字段字典序（覆盖绝大多数类型）；候选2 config_set_resp 4 字段子集。两者皆可完美对拍板端。
- 已有离线校验工具：`D:/Desktop/TCP_JSON_Sign_Tool/tcp_json_sign_tool.py`（+ exe，当前 v18：整窗浅色白底内容区），以及 `D:/Desktop/JSON_Sign_Tool.html`（纯前端，字典序排序+签名）。打包环境复用 `D:/Desktop/OTA_Upgrade_Tool/.venv_build` 的 PyInstaller；升级改版时递增版本号打包（exe 运行窗口会锁旧文件）。

## 用户协作习惯
- **改动前必须先确认**：对项目代码/配置/文档做任何修改（含新增/删除/编辑文件、编译构建等会改变项目状态的操作）前，必须先向用户给出方案、获批后再动手。只读分析、grep/搜索、内存与日志写入、用户已明确授权或明确要求直接执行的任务除外。该约定同时记于跨项目 `~/.workbuddy/MEMORY.md`。
