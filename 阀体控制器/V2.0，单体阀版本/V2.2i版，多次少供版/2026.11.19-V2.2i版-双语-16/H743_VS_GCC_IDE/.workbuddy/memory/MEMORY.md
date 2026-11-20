# 项目长期记忆（H743_VS_GCC_IDE / LUB-CTRL-V2.0i）

## 硬性约束
- Modbus 侧保持原样；以太网侧修改不得影响 Modbus。Modbus 侧 = syncif.c / FreeModbus / var_init.c；以太网侧 = net_comm_task.c / running_logic.c 注油部分。
- 共享状态 `main_sys_status`(running_logic.h:200)、`injector_config`(syncif.c:53) 双侧可见，写它们即改 Modbus 可见值。
- `injectRequest`(running_logic.h:142) 是 Modbus 可观测寄存器 300+tag*20+6，以太网勿动其语义。
- 跨工程/跨版本移植禁止整文件覆盖，只按 hunk/锚点合并（尤其 running_logic.c）。11-04 合并详见 `同步说明_11-04功能合并_20261110.md`。
- 本工程文件 100% CRLF；Python 替换后必须归一化并逐行 diff 确认区块外零改动。
- 单文件语法检查必须 `-std=gnu11`（否则 osThreadNew 误报）；验证用全新 `build/verify` + PATH 含 arm-none-eabi-。
- 改代码/配置/文档前先向用户确认方案（只读分析、内存与日志写入除外）。

## 身份与版本
- `DEVICE_MODEL` 加低速晶振后缀：i=LSI(本工程 LUB-CTRL-V2.0i)、e=LSE；`DEVICE_FW_VER=2.1.6`。上位机白名单需放行。
- 协议文档在 `d:\Desktop\R\工程\阀体控制器\协议\`，按 V2.x 命名，旧版本不覆盖；改动新建下一版本（基线 V2.7，删 task_request 后为 V2.8）。

## OLED 菜单（当前主线）
- 无空闲超时自动返回，一律长按返回（不移植参考工程 idle timeout）。
- 结构：一级6项；System Settings 二级7项(含 P/Q Model Init、P/Q Model Speed)；Serial Port 无三级菜单，短按直达波特率编辑页。
- 波特率 `main_config.BaudRate` 6档 9600..230400，保存 `sync_to_sysdb()`+`sync_to_modbus()`，mbif.c 启动读取需重启生效。以太网 IP 另属 `g_net_cfg`(net_comm_task.c static)，与 main_config 无关。
- P/Q Speed：选择/修改两层。选择态光标 `>`、修改态 `*`、非光标行留空。步进50下限200，Max上限3000、Target上限=max，max调小联动压base。存 `injector_config.maxMotorSpeed/baseMotorSpeed`，`sync_to_sysdb()`+`sync_to_modbus()`。暂不同步电机 buffer(motor_zdt.c 0xFB/0xFD 仍写死 0x08FF)。
- 中文化进行中：`cn_fonts.h` 提供 cn_font_12/16/24，必须 `u8g2_DrawUTF8()/GetUTF8Width()`（DrawStr 仅 ASCII）；U8G2_WITH_UNICODE 已启用。用户要求"先改一块看效果"，当前块=主菜单。
- 跨工程移植 OLED 禁整文件覆盖（参考工程 SYS_CONFIG_T 与本工程结构不同）。

## 供油策略（type3 分次注油）
- `total_x10`=总注油次数；`sub_total`=批数；`sub_index`=第几批；`inj_v`=本批次数。上行 execute_result 无 task_type、无 period_seq，sub_index/sub_total 原样回显。
- 每 task_order 帧仅 1 个 task_info（一批一帧），被动累积执行，不拿 sub_total 当闸门。
- 同一轮共享 task_id、以 sub_index 区分；去重键=复合(task_id,sub_index)，禁止退化为单 task_id（否则第2..K批被误杀）。
- 离线：整轮 total_x10 一次性注完，同 task_id 的 K 批全部 done+回收；`g_offline_round_sched` 管"整轮都在离线期"的后续轮次（首批发时间为基准）。
- 调度表 `g_sched_list[24]` 回收语义=同时挂起上限，非一生累计。
- 真实执行链 injector_task→inject_running 逐mL→inline_report_result→tcp_send_execute_result。`test_oil_filling_task` 是死代码(线程创建被注释、被gc回收)，勿当运行点讲解。

## 协议签名
- 入站按 key 字典序(strcmp 字节序)规范化验签；task_order 用 json_sort_keys 递归排序(cJSON 自实现，双链稳定 selection sort，超限放弃排序勿截断，注意 prev 维护)。外层5键手工字典序。
- PSK=`0123456789abcdef0123456789abcdef`(project_config.h:68)。验证基准 通讯实例.txt：task_order→4020F204、device_reboot→BE4CAC2D。
- 遗留：net_comm_task.c:2089-2099 时间窗校验+seq防重放两道闸仍注释未启用。

## OTA
- 丰富版协议：upgrade(crc hex串/filename/file_size)，`ota_handle_upgrade_start(len,crc,filename)` 三参，TFTP 文件名由 filename 决定。upgrade_result 未连网缓存 FlashDB(key ota_result)补发。OTA_FILENAME_MAX_LEN=20、OTA_TFTP_PORT=69。
- 卡死根因=LSE 晶振异常，已切 LSI 解决。CLOSE_WAIT 修复：仅 err!=ERR_OK 才清 tcp_pcb，优雅 FIN 交 safe_tcp_close()。

## ZDT 电机（Y42 第二代方言）
- 帧 [地址][功能码][辅助码?][数据][0x6B固定校验]；0x88/0x52/0x97 是辅助码非第二功能码。CAN 扩展帧 ID=(地址<<8)+包序号，>8字节分包每包首字节重复功能码。
- 位置模式自带速度字段随位置命令下发；0x08FF=2303≈230.3RPM(0.1RPM缩放，待波形确认)。0x45 0x66 设电流帧 len 应为6(当前4，无调用点)。
- ⚠️ `zdt_run_motor_loc_abs()`/_abs_t() 2处帧ID写成 `addr*ex_addr`（应为 ex_addr），地址≥2会发错，用户决定暂不改、仅注释存档。

## OLED 中英文双语（11-19 新增，仅主工程）
- `oled_lang.h` 提供 `g_oled_lang` + `OLANG(cn,en)`；英文文案一律复用汉化前原厂串，不自创翻译。切换入口=系统设置二级菜单「语言/Language」，短按切换+持久化。
- 语言存 FlashDB 独立 key `oled_lang`（"0"/"1"），经 `syncif.c` 的 `sysdb_get_u8/set_u8`；**不进 main_config、不碰 Modbus 映射**。菜单名两张表运行时改 `item_name` 指针，`g_menu_items` 仍是数组。
- ⚠️ C 全局初始化器必须常量：菜单数组不能用 OLANG，只能字面量初始化后由 `oled_lang_apply_menu()` 覆盖。衍生版未同步此功能。
- 英文模式必须**同时**还原原厂的字体、基线坐标与排版结构：标题/品牌=`u8g2_font_ncenB14_tr`、正文=`u8g2_font_9x15_tf`、编辑页值与速度行=`u8g2_font_10x20_tf`、重启文案=`u8g2_font_ncenB18_tr`、状态栏=`u8g2_font_6x10_tf`；中文用 `cn_font_*`+DrawUTF8。统一走 `draw_lang_text(...,cn_y,en_y)/lang_text_width()` 原语切换（**中英各一套基线**）。改字库必须同步 `cn_font_gen.py` 字符表并重跑 `python cn_font_gen.py`（重写 cn_fonts.c/.h，ASCII 段应保持不变）。
