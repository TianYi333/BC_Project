# 项目长期记忆

## 架构约束：以太网侧与 Modbus 侧隔离（2026-11-03 用户明确）
- **硬性要求**：Modbus 侧逻辑保持原样不动；**以后以太网侧的修改都不得影响 Modbus 侧**。
- **两者的边界**：
  - Modbus 侧 = `UserCode/Logic/syncif.c` + `Middlewares/FreeModbus` + `UserCode/Logic/var_init.c`（寄存器映射、同步、FlashDB 配置存储）。
  - 以太网侧 = `UserCode/Drivers/ETH/net_comm_task.c` + `UserCode/Logic/running_logic.c` 的注油任务相关部分。
- **共享状态 = `main_sys_status`**（`running_logic.h:200`）与 **`injector_config`**（`syncif.c:53`）——
  两者都是双侧可见，故以太网侧写它们即等于改 Modbus 可见值。
- **已知现存违反点（唯一一处，待用户定夺）**：以太网 type=2（周期任务）在 `net_comm_task.c:3504` 调用
  `sync_config_file()` → 把 `injector_config` 的 interval/volume/enable 同步进 `main_sys_status.injector[]`
  → Modbus 寄存器 `300+tag*20+1..5` 读到的值随之改变。
  （以太网侧**不直接**写 `main_sys_status.injector[]` 的配置类字段 —— 已 grep 确认。）
- **Modbus 侧不涉及 task_request**：`syncif.c` / FreeModbus 全无 `task_request` / `pending_requests` /
  `xTimerReqQueue` 引用 → 上行任务请求的删除对 Modbus 零影响。
- **`injectRequest` 字段是 Modbus 可观测状态**（`running_logic.h:142`）：
  `syncif.c:344` 映射到寄存器 `300+tag*20+6`（读）、`syncif.c:548` 从该寄存器回写（写）。
  以太网侧**不读写**它（仅 `net_comm_task.c:1695/1701` 注释提到）→ 以太网改动勿动此字段语义。

## 协议文档版本约定（2026-11-03 用户明确）
- 协议文档在 `d:\Desktop\R\工程\阀体控制器\协议\`，按 `以太网通讯协议设计说明文档 V2.x.txt` 命名，
  历史版本 V2.0/2.1/2.4/2.5/2.6/2.7 全部保留（不覆盖）。
- **涉及协议修改时新建下一个版本号**（例：删 `task_request` → 新建 **V2.8**），**不改旧版本文件**。
- V2.7 基线：`以太网通讯协议设计说明文档 V2.7.txt`，1992 行，CRLF 行尾，106432 B。

## 项目关系
- 当前项目 `H743_VS_GCC_IDE`（低速内部晶振版，`d:\Desktop\temporary\2026.10.08-V2.0版，低速内部晶振\`）是参考项目 `d:\Desktop\H743_VS_GCC_IDE` 的低速时钟源变体。
- 两项目唯一功能差异：低速时钟源相关文件 `Core/Src/{gpio.c,main.c,rtc.c}` 与 `H743_VS_GCC_IDE.ioc`。其余（HAL 库、CMake 配置、OTA 逻辑）应保持一致。
- 跨项目移植时：只动目标差异文件，时钟文件切勿改动。
- **型号/版本命名（2026-11-10 用户定）**：`DEVICE_MODEL` 按低速晶振源加后缀 ——
  `i`=LSI 内部（本工程 `LUB-CTRL-V2.0i`）、`e`=LSE 外部（外部晶振版 `LUB-CTRL-V2.0e`）；
  当前 `DEVICE_FW_VER=2.1.6`。register/heartbeat 上报的 model/ver 随之变，上位机白名单需放行。
- **11-04 功能同步（2026-11-10 完成）**：从外部晶振版按 hunk 合并 4 类缺陷修复
  （`waiting_met_sen` 5 档返回码 + 判据 `==1`、失败诊断串 `p/g/r`、`g_q/g_p_zero_ready` 归零阻断、
  错误描述改诊断串）。⚠️ **禁止整文件复制 `running_logic.c`**：外部晶振版仍留
  `xTimerReqQueue` 入队块（V2.8 已删）而本工程有 5 个以太网侧独有 hunk；合并后与源改后版
  仅余这 5 个 hunk 差异。详见 `同步说明_11-04功能合并_20261110.md`，备份 `_backup/*.bak_sync1104`。
  测试模式下 `injector_task` 未创建 → 归零/定位逻辑编译通过但不执行，真机验证须切回生产模式。

## OTA 实现（2026-10-12 整体同步至参考项目）
- OTA 采用「丰富版」协议：命令 `upgrade`（`crc` hex串 / `filename` / `file_size`），`ota_handle_upgrade_start(uint32_t len, uint32_t crc, const char* filename)` 三参；TFTP 文件名由命令下发的 filename 决定（`g_ota_exp_fname`）。
- 含 `upgrade_status` / `upgrade_result` 上报（`net_comm_task.c` 实现）；`upgrade_result` 未连网时缓存到 FlashDB（key `ota_result`），连上后由 `upgrade_result_check_and_report()` 补发；旧版本号存 `ota_oldver`。
- 协议常量 `OTA_FILENAME_MAX_LEN=20`、`OTA_TFTP_PORT=69`（见 `ota_common.h`）。
- 注意：协议再次变更时，须同步上位机/推送脚本（`ota_tftp_push.py` 等）。

## 构建注意
- `build/Debug` 的 CMakeCache 指向参考项目路径，不可直接用于验证当前项目；验证当前项目须用全新 build 目录（如 `build/verify`）并配置 `cmake --preset Debug -B build/verify`，且 PATH 含 `arm-none-eabi-`（如 `/d/APP/GCC-ARM/bin`）。

## OTA 卡死根因（2026-10-13 用户确认）
- 升级后卡在 `[BOOT] verify ok / meta=2 written` 乃至 `[BOOT] HARDFAULT` 的根因是 **LSE 外部低速晶振异常**，非 Boot 跳转/烧录逻辑问题；切到 **LSI 内部低速时钟** 后解决。
- 本工作区 `d:\Desktop\H743_VS_GCC_IDE` 当前已是 LSI（`Core/Src/main.c:176-178` + `Core/Src/rtc.c:111`），无需再做时钟切换。
- CLOSE_WAIT 孤儿连接修复：仅当 `err != ERR_OK` 才清空 `tcp_pcb`；优雅 FIN 保留 `tcp_pcb` 交 `safe_tcp_close()` 关闭（见 `net_comm_task.c` `tcp_recv_cb`）。

## 供油策略契约（type3 分次注油）三层语义（2026-10-28 用户澄清）
- 整轮：`total_x10` = **总注油次数(count)**，如 15 次；`sub_total` = 拆成几批；`sub_index` = 第几批；`inj_ts` = 批间隔；`inj_v` = **本批连续注油量(=本批注油次数)**，如 3。→ `total_x10 = sub_total × 每批次数`。
- 上行 `execute_result`：`sub_index/sub_total` = 原样回显下行（仅 type3 在线）。**无 `period_seq` 字段**（已按用户要求去掉：1ml 阀体下与 `actual_volume` 数值重复）；批内「第几次注油」由网关按 `sub_index` + `actual_volume`（每完成 1 次动作递增 1）自行推算。
- 离线降级（断联）：**同一轮共享同一 `task_id`、各批以 `sub_index` 区分**（契约 §5.1/§6.2，2026-10-29 用户最终确认；此前"每批唯一 task_id"为误述已纠正）→ 离线按**整轮 `total_x10` 一次性注完**（固件拿不到逐批节奏的兜底设计），触发后同 `task_id` 的 K 批全部标记 done+回收，防每批各注整轮 = M 倍超注；`total_x10` 由网关按阀体规格换算，固件直接使用。非分次(type1/2/4)也不带 sub_index/sub_total。
- **上行 `execute_result` 已移除 `task_type`（2026-10-29 用户要求）**：协议 V2.7 的 `execute_result` 字段表本就无 `task_type`（只有下行 `task_order` 有），故属回归协议原样。已删 `ExecuteResultInfo_t.task_type` 字段（`net_comm_task.h`）、组包处 `cJSON_AddNumberToObject(item,"task_type",...)`（`net_comm_task.c`）、`inline_report_result` 里 `exec_info.task_type=` 赋值（`running_logic.c`）。`g_tcp_task_bind.task_type` **保留**（内部仍记录下发类型，仅不再上行）。分次回报 `sub_index/sub_total` 的输出判据是 `sub_total>0`、**不依赖 task_type**，不受影响。
- **离线周期整批注油（覆盖"整轮都发生在离线期"的后续轮次，2026-10-29 实现）**：前述 g_sched_list 离线整轮分支**仅覆盖"本轮已收批在离线态到达 `exec_at`"**；若一整轮都发生在离线期（收不到任何 `task_order` 批），`g_sched_list` 无该轮表项、调度器不触发 → 离线期后续轮次完全不注油。新增全局 `g_offline_round_sched{valid,task_type,inj_id,total_x10,adj_period_sec,task_id[TASK_ID_LEN],last_fire_unix}`：在线收到首个 `task_type=3` 批时捕获本轮配置（轮界=首次见到该 task_id；`sub_index==1` 的 `exec_at` 校正轮起点），离线态每逾 `adj_period_sec` 自行整批注 `total_x10` 一次（不写 g_sched_list、不回显 sub、离线发送被 `g_tcp_connected` 守卫丢弃故不回报），随后推进 `last_fire_unix` 防同轮重注/爆发补注。与 g_sched_list 离线分支互补：后者管"本轮"、前者管"后续整轮"。参考时刻取"本轮首批发时间"（用户确认）。
- **使用习惯（用户 2026-10-29 明确）**：每个 `task_order` 报文**仅含 1 个 `task_info` 元素**（一批=一帧），分批注油时一次只下发一批、不会一次下发多批。固件按帧被动累积执行（来一份跑一份），不拿 `sub_total` 当完整性闸门——网关发几批就执行几批，不发不执行。
- 调度表 `g_sched_list[SCHED_MAX=24]`（2026-10-29 方案A）：各格执行完毕（在线份回报 / 离线逐批安排）后由调度器回收 `valid=0`，**语义为"同时挂起份数上限"非设备一生累计**；与 `(task_id, sub_index)` 复合键去重（`dedup_mark` **受理成功后**才落环：type=3=入表成功、type=1/2/4=入队成功）互不冲突，回收不会造成已执行批重发再注。
- **去重键 = 复合键 `(task_id, sub_index)`，必需、禁止退化为单 `task_id` 键**（2026-10-29 澄清+回退后确认）：**主理由=同一轮共享 `task_id`、各批以 `sub_index` 区分**，若退化为单 `task_id` 键，一轮内第 2..K 批会因"task_id 已存在"被误杀，整轮只剩首批——正常功能不可用。**次理由=兜底 `generateTaskID` 同纳秒碰撞**（两个不同轮次同 task_id）：复合键将该碰撞轮次整轮拦下（偏安全漏注，而非同 id 整轮重复注油）。仅真正重传（task_id 与 sub_index 均同）被拦下。§4.2-6 / §6.2-2 已登记。固件**依赖**"同一轮同 task_id"前提（这正是设计基础，与契约 §5.1/§6.2 一致）。
- **（已回退 2026-10-29）离线逐批 `inj_v` 为误改**：当时误信"每批唯一 task_id"，将离线从"整轮 `total_x10`"改为逐批 `inj_v`。用户随后纠正——同一轮同 `task_id`、仅 `sub_index` 不同（与契约一致），离线本就该整轮兜底。已回退 `net_comm_task.c` 离线分支：`val` 恢复 `s->total_x10`、同 `task_id` 的 K 批 done+回收逻辑恢复。所谓"超注"从未真实存在（整轮兜底正是设计意图）。
- 实现要点：`inject_running()` 的 `for(tag=inject_val; tag>0; tag--)` 每 1ml 动作单独回报一次 `execute_result=1`，`fill_cur` 即本批已完成次数（`actual_volume = fill_cur`）。
- 真实驱动路径：`task_object_list[3]` → `injector_task()` SYS_MODE_READY/MONITOR → `inject_running` → `inline_report_result` → `tcp_send_execute_result`；模拟任务 `test_oil_filling_task` 已禁用(死代码，用户决定保留原样)。
- ⚠️ **`test_oil_filling_task` 绝不可当作"运行点"讲解**（用户 2026-11-03 再次纠正，AI 讲解时又踩了同一个坑）：
  它是**未启用的实验点/死代码** —— ① 线程创建 `osThreadNew` 在 `Core/Src/freertos.c:385` **被注释掉**；
  ② 链接期已被回收：`build/verify/H743_VS_GCC_IDE.map` 中 `.text.test_oil_filling_task` 落在
  **Discarded input sections** 段（地址列全 0），运行时零成本。
  **真实执行链**：`injector_task()`（`UserCode/Logic/running_logic.c`，由 `start_injector_task()` 创建，
  `freertos.c:319`）→ `SYS_MODE_READY` 分支扫 `task_object_list[0..4]`，命中 `task_status==1`
  → 调 `inject_running(val, inject_id)`（`running_logic.c:445`）→ 内部**逐 mL** 循环
  （`fill_cur++` 后 `inline_report_result(..., fill_cur, ..., 1, 0, "OK")`，即每 1mL 一次 `execute_result=1`
  中间回报，`actual_volume = fill_cur`）→ 完成后 `g_slot3_ctx.valid = 0` 清分次回报上下文。
  逐 mL 回报由**真实驱动**发出，不是由 test_oil_filling_task 发出。
- 契约文档：`d:\Desktop\R\工程\阀体控制器\供油策略阀体待实现功能说明(1).md`（改动需与本文档同步）。

## 协议签名（2026-11-02 新增，方案 B）
- **入站验签按 key 字典序规范化**：`task_order` 分支在 `cJSON_Duplicate` 后调 `json_sort_keys(arr_copy)` —— 对象键按 `strcmp` 字节序**稳定**排序、**数组元素顺序不动**、嵌套对象递归。依据协议 V2.7 §2.4.1「所有字段按key字母顺序排序」。
- **cJSON 无内置排序**，排序函数为本项目自实现（`net_comm_task.c`：`json_key_cmp`/`json_sort_object_children`/`json_sort_keys`）。**关键坑**：cJSON 对象子节点是 `child`/`next`/`prev` **双向链表**，重排后必须同步维护 `prev`，否则 `cJSON_Delete` 遍历会漏释放。
- **排序算法必须用「两条独立链的稳定 selection sort」**（2026-11-03 修正）：`rest`(未排序) / `out`(已排序) 两条链互不遍历。
  ⚠️ **在链表上"遍历中修改"极易成环死循环** —— 试过「就地摘链插入」（搜索位置会扫回刚插入的节点自身）与「冒泡相邻交换」（两节点来回换）**均死循环**。
  ⚠️ **定长数组做"收集-重建"时超限必须"放弃排序、原样返回"，绝不能截断** —— 截断会静默丢字段 + 泄漏
  （`cJSON_Delete` 只沿 `next` 遍历，被孤立的节点永不释放；cJSON 走 `pvPortMalloc`）。
- **只能用 `strcmp`（字节序、区分大小写）**，不可用 cJSON 内部的 `cJSON_strcasecmp`（大小写不敏感，仅用于 `cJSON_GetObjectItem` 查表）。网关侧 Go 的 `sort.Strings`/`json.Marshal(map)` 是字节序 → 必须一致。
- **外层 5 键仍手工字典序**（device_id→seq→task_info→ts→type），只有 `task_info` 数组元素内部走递归排序。`task_order` 是**唯一含数组的入站分支**，其余入站分支为手工拼串、无数组 → 行为不变。
- **改动前是"碰巧对"**：真实网关帧内层本就是字典序，故新旧行为结果一致；一旦网关调整字段顺序，旧实现会整帧验签失败、任务被静默丢弃。
- **验证基准**：`d:\Desktop\R\工程\阀体控制器\通讯实例.txt` 记录了真实网关帧与签名 `4020F204`（对应**含 `remaining`** 的变体；该文件 "Sorted task_order" 行漏写了 `remaining` 字段，是手写示例、与实际签名串不一致）。PSK = `0123456789abcdef0123456789abcdef`（`UserCode/Logic/project_config.h:68`）。
- **遗留**：`net_comm_task.c:2089-2099` 的**时间窗校验与 seq 防重放两道闸仍被注释掉**（`// goto exit;`）—— 签名对了就放行，不检查过期/重放。放开前须确认网关每次发新 seq + 合法 ts，否则误杀正常帧。
- **全量审计（2026-11-03）**：9 个入站分支（TCP 7 + UDP 2）签名串**键序均严格字典序**、**字段集合与报文一致**；
  端到端闭环：`task_order`→`4020F204`、`device_reboot`→`BE4CAC2D` 与 `通讯实例.txt` 记录值逐位吻合。
  另 3 个风险点（当前字段集不触发）：① Go `json.Marshal` 会 HTML 转义 `<>&` 而 cJSON 不会；
  ② cJSON 非整数用 `%f`（`1.5`→`1.500000`）与 Go 最短表示不同 —— 但协议字段全为 `int`/`uint16_t`；
  ③ 手工拼串分支若网关新增字段则验签必失败。

## OLED 菜单约定（2026-11-18 用户明确）
- **本工程不要空闲超时自动返回**：参考工程 `Oil_Pump_Control` 的
  `menu_reset_idle_timer()/menu_is_idle_timeout()`（10s 回首页）**不移植**；
  菜单一律"长按返回、无自动退出"。以后做 OLED 菜单勿主动加超时。
- 菜单结构（11-18 实现后）：一级 6 项；`System Settings` 二级 7 项
  （含从主菜单迁入的 `P Model Init`/`Q Model Init`）；
  **`Serial Port` 无三级菜单——短按直接进入串口波特率编辑页**（用户 11-18 纠正，
  只保留波特率修改，删掉原 8 项通讯参数：Modbus Addr/IP/掩码/网关/Server IP/端口）。
- 波特率编辑 `main_config.BaudRate`（Modbus 保持寄存器可见），6 档 9600…230400，
  旋转选档/短按保存/长按返回；保存走 `sync_to_sysdb()` + `sync_to_modbus()`；
  由 `mbif.c` 启动时读取，需重启生效。
  **以太网实际 IP 是另一套 `g_net_cfg`（`net_comm_task.c` static，KV `net_cfg`）**，
  与 `main_config` 的 IP 类字段无同步关系，OLED 菜单不改动 lwIP 地址。
- **`P/Q Model Speed` 编辑页（11-18 改版后）**：**选择/修改两层交互**（用户明确要求，
  参考 Oil_Pump_Control 的 PID P/I/D 设置页显示风格 `>` 前缀，但交互按用户描述做成两层）：
  进入页 → 旋转选择「Max Speed / Target Speed」→ 短按进入修改 → 旋转调值 → 短按确认 → 长按返回。
  直接读写 `injector_config.maxMotorSpeed` 与 `baseMotorSpeed`（不新增字段，不动 Modbus 结构）。
  步进 50、下限 200；**上限按项区分：最大速度项用绝对上限 `MOTOR_SPEED_MAX_RPM`(3000)，
  目标速度项以 maxMotorSpeed 为上限**（⚠️ 两者都取 max 会让"最大速度"项被自身钳死、无法增大）；
  **max 调小到低于 base 时同步压低 base**。
  选中标记（**用户两次纠正后定稿，勿再弄反**）：**`>` 是光标标记**（选择态，光标行显示 '>'，
  旋转上下移动光标时跟着移动）；**短按进入修改态后该行由 '>' 变为 '*'**；
  非光标行一律留空（无任何前缀）。修改态短按确认保存后退回选择态，'*' 变回 '>'。
  保存走 `sync_to_sysdb()`（落 FlashDB 掉电保留）+ `sync_to_modbus()`（刷新寄存器 209/210、211/212）。
  P/Q 两个菜单入口共用同一编辑器。显示只用 9x15 字体单风格，行距 26px、反白框紧贴文字（**行距必须 > 字体高，
  否则下一行反白框会压住上一行文字** —— 曾因此显示错乱）。
  **当前不同步到电机下发 buffer**——`motor_zdt.c` 的 0xFB/0xFD 速度字段仍写死 `0x08FF`。
  后续若要生效：改 `motor_zdt.c`（0xFB 在 `buffer[2]/[3]`、0xFD 在 `buffer[6]/[7]`）读本值。
- ⚠️ **改本工程文件必须保持 CRLF**：原文件 100% CRLF，用 Python 批量替换时新块易混入 LF，
  会造成全文件行尾混杂。替换后须做 bytes 级归一化并逐行 diff 确认区块外零改动。
- **ZDT 电机协议要点（11-18 查证官方语雀文档站 `yuque.com/zhangdatouzhikong/gzng3d`）**：
  - 帧结构 `[地址][功能码][辅助码?][数据][校验0x6B]`；**0x6B 是固定校验字节**（非帧尾），
    0x88/0x52/0x97 等是**辅助码**（细分动作），不是"第二个功能码"。
  - CAN：扩展帧，**帧ID = (地址<<8)+包序号**；>8 字节分包，**每包首字节重复功能码**，
    末包带 0x6B。项目 `motor_zdt.c` 分包方式与此一致。
  - 本工程电机 = **Y42 第二代**方言（位置角 0.1°、速度/加速度各 2 字节，`0xFD` 共 15 字节），
    与 Emm 方言（13 字节、脉冲数、1 字节加速度档）**不通用**，别拿 Emm 手册推断。
  - 已核对吻合：0x36 读位置（跳过符号字节取 4 字节再 /10）、0x93 0x88、0x9A、0x0E 0x52、
    0x08 0x97（重启，Y42 专有）、0x45 0x66（闭环最大电流）。
  - `0x08FF` = 2303，按 Y42 方言约为 **230.3 RPM**（0.1RPM 缩放）——单位待抓波形最终确认。
  - 位置模式**自带速度字段**，速度随位置命令一起下发，不需独立速度命令；
    想改运行中速度靠"位置模式命令打断"（再发一条位置命令）。
    唯一能持久化速度的是 `0xF7 0x1C` 上电自动运行（存进电机，掉电保留，上电自转）。
  - ⚠️ `zdt_set_motor_ma()` 的 `len=4` 应为 6（规范 `45 66 01 13 88 6B`），
    当前截断导致电流低字节与校验字节没发出去（该函数目前无调用点）。
  - ⚠️ `motor_zdt.c` **2 处**帧 ID 写成 `addr * ex_addr`（多乘一次 addr，应为 `ex_addr`）：
    `zdt_run_motor_loc_abs()` 与 `zdt_run_motor_loc_abs_t()`。
    当前地址均为 1 故恰好等价；地址改 ≥2 会导致首包发往错误 ID、电机无响应。
    **用户 11-18 决定：暂不修改，仅在代码内注释存档。**

- 跨工程移植 OLED 菜单时禁止整文件覆盖：参考工程 3548 行且参数结构完全不同
  （`SYS_CONFIG_T` vs 本工程 `main_config`/`injector_config`），只能按 hunk 取通用框架。
