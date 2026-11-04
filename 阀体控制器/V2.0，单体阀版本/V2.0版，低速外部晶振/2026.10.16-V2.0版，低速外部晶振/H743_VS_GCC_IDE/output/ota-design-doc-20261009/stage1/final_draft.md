# 控制节点（注油控制器）固件远程升级（OTA）系统设计说明

> 版本 V1.0　编制日期 2026-10-09　适用范围：STM32H743ZGT6 注油控制器（Oil_Pump_Control 工程）
> 配套代码：App 工程 `H743_VS_GCC_IDE`、`/d/Desktop/boot`（Boot 工程）、`/d/Desktop/OTA_Upgrade_Tool/ota_tftp_push.py`（上位机脚本）

## 文档信息

| 项目 | 内容 |
|------|------|
| 文档名称 | 控制节点（注油控制器）固件远程升级（OTA）系统设计说明 |
| 版本 | V1.0 |
| 适用设备 | STM32H743ZGT6 注油控制器（Oil_Pump_Control） |
| 运行环境 | FreeRTOS + lwIP(TCP/Modbus) + 外部 W25Q64(8MB) + 内部双 Bank Flash |
| 文档性质 | 设计说明（已实现可用） |

## 一、概述

### 1.1 背景与目标

工业注油控制节点分布广、现场升级成本高。本设计实现设备固件的**远程在线升级（OTA）**，目标是在不停机、不拆机的前提下，通过以太网将新固件安全、可靠地下发到控制节点并完成烧录切换。

设计的核心是**防砖（brick-proof）**：任何升级失败（断电、固件损坏、校验失败、新固件起不来）都不能让设备永久变砖，必须能自动回退到可运行的旧固件。

### 1.2 设计目标

- **A/B 防砖**：升级前先把当前可用 App 备份到外部 Flash，烧录失败自动回退。
- **断电可恢复**：升级状态持久化在外部 SPI Flash 的 `ota_meta` 区，断电/异常重启后能续跑或回滚。
- **固件完整性校验**：固件采用 CRC32 双重校验（外部 Flash 一次、烧录后内部 Flash 再校验一次）。
- **指令防伪造**：控制指令采用 HMAC-SHA256 签名，拒绝非法/伪造的升级命令。
- **低侵入**：升级链路独立于业务任务，App 接收、Boot 烧录职责清晰。

### 1.3 适用范围

| 项 | 说明 |
|----|------|
| 设备类型 | 注油控制器（Oil_Pump_Control，STM32H743ZGT6） |
| 通信协议 | 控制通道 TCP（指令）+ 文件通道 TFTP（固件） |
| 网络环境 | 以太网（eth0） |
| TFTP 角色 | 设备为 Server（UDP/69），上位机/网关为 Client（主动 put） |

### 1.4 实现状态对照

本协议与流程**已在当前工程中完整实现并可用**，区别于参考文档中"网关推送链路待开发"的占位项。下表对照参考文档 V2.1 的"实现状态"栏：

| 功能点 | 本项目状态 |
|--------|-----------|
| 设备侧 TFTP Server（UDP/69） | 已实现（`ota.c` + lwIP `tftp_server`） |
| 设备为 TFTP Server、上位机为 Client | 已实现（与参考文档一致） |
| TCP 指令下发 + ack | 已实现（`upgrade_start` / `upgrade_start_ack`） |
| 固件落盘 + 完整性校验 | 已实现（CRC32 双重校验） |
| 烧录 + 防砖回滚 | 已实现（Boot `do_update` + `firmware_bak` 备份） |
| HMAC 指令签名 | 已实现（HMAC-SHA256，取摘要前 4 字节） |

## 二、系统架构

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                     上位机 ota_tftp_push.py                            │
│  ① TCP 客户端：连接设备 TCP Server 端口 50010，发 upgrade_start      │
│  ② TFTP 客户端：主动 put 固件到设备 TFTP Server（UDP 69）            │
└───────────────────────────┬────────────────────────────────────────┘
                              │ TCP(50010) upgrade_start / ack
                              │ TFTP(UDP/69) 固件推送
                              ▼
┌──────────────────────────────────────────────────────────────────────┐
│                         控制节点设备（H743）                          │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ App 工程（运行态）                                               │  │
│  │  net_comm_task.c：TCP 收 upgrade_start → HMAC 校验               │  │
│  │  ota.c：擦 firmware_a → 启 TFTP Server → 收块写 SPI + 增量CRC   │  │
│  │        → 收满校验 → 写 ota_meta(PENDING) → 软复位               │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                              │ 复位                                   │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ Boot 工程（0x08000000，上电先跑）                                │  │
│  │  main_boot.c：读 ota_meta → do_update                          │  │
│  │   校验 firmware_a CRC → 备份当前 App 到 firmware_bak            │  │
│  │   → 烧录内部 Flash BANK2 → 校验 → 置 DONE → 跳 App            │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                              │                                        │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ 外部 W25Q64（8MB，fal 分区）                                    │  │
│  │  firmware_a(1MB)  firmware_bak(1MB)  ota_meta(64KB)            │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| OTA 接收/确认 | `UserCode/Drivers/ota/ota.c`、`ota.h`、`ota_common.h` | App 侧接收 TFTP 固件、写 meta、启动确认（防砖确认点） |
| 控制通道 | `UserCode/Drivers/ETH/net_comm_task.c` | TCP 收 `upgrade_start`、HMAC 校验、回 `upgrade_start_ack` |
| TFTP 服务 | lwIP `tftp_server`（App 调用 `tftp_init`） | 监听 UDP/69，接收固件块并回调 `ota.c` |
| 分区管理 | `UserCode/Third_Party/Fal/fal_cfg.h` | 定义 W25Q64 的 fal 分区表 |
| Boot 烧录 | `/d/Desktop/boot/Core/Src/main_boot.c` | 读 meta、校验、备份、烧录内部 Flash、回滚、跳转 |
| 上位机脚本 | `/d/Desktop/OTA_Upgrade_Tool/ota_tftp_push.py` | 发 `upgrade_start`（TCP+HMAC）+ TFTP put 固件 |

### 2.3 TFTP 角色定义（关键）

- **设备 = TFTP Server**：监听 UDP/69，仅在升级窗口内由 `ota.c` 调用 `tftp_init()` 临时拉起，接收完成后 `tftp_cleanup()` 关闭。
- **上位机/网关 = TFTP Client**：收到 `upgrade_start_ack` 后，主动将固件 `put` 到设备的 TFTP Server。
- **先指令后文件**：`upgrade_start`（TCP）先下发，设备确认并启动 TFTP Server 后，上位机才主动推送固件（TFTP）。这是与"网关被动等连接"相反的主动推送模型。

## 三、存储分区与状态机

### 3.1 外部 W25Q64 分区布局（fal）

定义于 `fal_cfg.h`，总容量 8MB。OTA 相关三个分区：

| 分区名 | 偏移（十六进制） | 偏移（十进制） | 大小 | 用途 |
|--------|-----------------|---------------|------|------|
| `firmware_a` | `0x30000` | 192 KB | 1 MB | 暂存本次新固件（升级接收目标） |
| `firmware_bak` | `0x130000` | 1216 KB | 1 MB | 升级前备份当前 App（回滚源） |
| `ota_meta` | `0x230000` | 2240 KB | 64 KB | 升级状态机（magic + state + len + crc） |

> 注：前 192KB 还包含 `ef_kvdb1/2/3`（各 64KB，FlashDB/KV 数据库），与 OTA 分区互不重叠。

### 3.2 内部 Flash 双 Bank 布局

STM32H743ZGT6 内部 Flash 1MB，双 Bank 各 512KB：

| 区域 | 地址 | 大小 | 说明 |
|------|------|------|------|
| Bootloader | `0x08000000` | 64 KB | 烧在低端，上电先跑 |
| App（BANK2） | `0x08100000` | 512 KB | 业务固件运行区（`OTA_APP_ADDRESS`） |

`OTA_APP_MAX_SIZE = 0x00080000 = 512KB`，即 App 固件上限。

### 3.3 OTA 状态机

`ota_meta_t` 定义于 `ota_common.h`，App 与 Boot **共用同一份语义**：

```c
typedef struct {
    uint32_t magic;       /* 0x4C55424F ("LUBO")，有效升级块标识 */
    uint32_t state;       /* 状态机当前值 */
    uint32_t new_len;     /* 新固件字节数（≤512KB） */
    uint32_t new_crc32;   /* 新固件 CRC32（与 fdb_calc_crc32 一致） */
    uint32_t reserved[4];
} ota_meta_t;             /* 共 32 字节 */
```

状态枚举：

| state | 宏 | 含义 | 写入方 | 下一步 |
|-------|----|------|--------|--------|
| 0 | `OTA_STATE_NONE` | 无待升级 | 初始/确认后 | — |
| 1 | `OTA_STATE_PENDING` | `firmware_a` 就绪，待 Boot 烧录 | App 收尾写 | Boot 烧录 |
| 2 | `OTA_STATE_DONE` | Boot 已烧录，待 App 启动确认 | Boot 烧录成功写 | App 确认 |
| 3 | `OTA_STATE_CONFIRMED` | App 启动成功，升级完成 | App 确认点写 | 闭环 |
| 4 | `OTA_STATE_FAILED` | 烧录/校验失败 | Boot 写 | 回滚 |

## 四、通信协议设计

### 4.1 协议层次

```
┌─────────────────────────────────────┐
│  应用层 (JSON)                       │
│  upgrade_start / upgrade_start_ack   │
├─────────────────────────────────────┤
│  传输层 (TCP, 端口 50010)            │
│  可靠连接，指令交互 + HMAC 签名      │
├─────────────────────────────────────┤
│  文件传输层 (TFTP / UDP, 端口 69)    │
│  上位机 Client 主动 put 固件到设备   │
└─────────────────────────────────────┘
```

### 4.2 控制通道（TCP，端口 50010）

设备作为 TCP 客户端连接上位机的 TCP Server（端口 `TCP_SERVER_PORT = 50010`，见 `net_comm_task.h:40`）。

#### 4.2.1 upgrade_start（上位机 → 设备）

上位机下发升级指令，通知设备准备接收固件。设备收到后校验 HMAC、擦 `firmware_a`、启动 TFTP Server，再回复 `upgrade_start_ack`。

```json
{
  "device_id": "1A2B3C4D5E6F",
  "new_crc32": 305419896,
  "new_len": 262144,
  "seq": 1001,
  "ts": 1746601200,
  "type": "upgrade_start",
  "sign": "7F2D91C0"
}
```

字段说明：

| 字段 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 设备 ID（由芯片 UID 派生，12 位 hex） |
| `new_crc32` | uint | 新固件 CRC32（标准 CRC-32） |
| `new_len` | uint | 新固件字节数（须 0 < len ≤ 512KB） |
| `seq` | uint | 命令序号 |
| `ts` | uint64 | 时间戳（十进制串，无前导零） |
| `type` | string | 固定 `"upgrade_start"` |
| `sign` | string | HMAC-SHA256 摘要前 4 字节的 8 位大写 hex |

#### 4.2.2 upgrade_start_ack（设备 → 上位机）

设备确认后回复（见 `net_comm_task.c:2676`）：

```json
{
  "device_id": "1A2B3C4D5E6F",
  "estimated_duration": 60,
  "message": "upgrade_start_accepted",
  "seq": 2001,
  "status": 0,
  "ts": 1746601201,
  "type": "upgrade_start_ack",
  "sign": "3A7F2D11"
}
```

| 字段 | 说明 |
|------|------|
| `status` | 0 = 接受；1 = 拒绝 |
| `estimated_duration` | 预计耗时（秒），含擦 `firmware_a`（约数秒到二十余秒） |
| `message` | `"upgrade_start_accepted"` 或 `"upgrade_start_rejected"` |

### 4.3 文件传输通道（TFTP，UDP/69）

- 协议：RFC1350 TFTP，`octet` 二进制模式，512 字节/数据块，每块需 ACK。
- 方向：上位机 Client 主动 `put`（`WRQ`），设备 Server 接收。
- 文件名校验：设备 `tftp_open_cb` 强制校验文件名 == `OTA_TFTP_FILENAME`（设备端为 `"H743_VS_GCC_IDE.bin"`，见 `ota.h:11`），且只接受写请求（`write != 0`），否则拒绝——防止误写。
- 端口说明：69 是握手端口，实际数据在临时端口间传输；抓包需同时关注设备临时端口。

### 4.4 拒绝原因（status=1 场景）

| 返回码（内部） | 含义 | 对应 ack |
|---------------|------|---------|
| -1 | 正在升级中（`g_ota_state != IDLE`），拒绝重入 | rejected |
| -2 | 长度非法（`new_len==0` 或 > 512KB） | rejected |
| -3 | CRC 非法（`new_crc32==0`） | rejected |
| -4 | 分区缺失（`firmware_a`/`ota_meta` 未找到） | rejected |
| -5 | `tftp_init` 启动失败 | rejected |

## 五、校验与安全机制

### 5.1 指令签名 HMAC-SHA256

目的：防止伪造/重放的升级命令。设备端实现见 `net_comm_task.c:2258-2271`。

- **签名源串（严格字典序）**：
  ```
  {"device_id":"%s","new_crc32":%lu,"new_len":%lu,"seq":%lu,"ts":%s,"type":"upgrade_start"}
  ```
- **算法**：HMAC-SHA256，密钥 `HMAC_KEY`（默认 `"0123456789abcdef0123456789abcdef"`，见 `project_config.h`）。
- **签名值**：取摘要前 4 字节，转 8 位大写 hex 字符串，作为 `sign` 字段。
- 设备收到后按同一算法本地计算并比对，不一致则 `goto exit` 丢弃该命令。

### 5.2 固件完整性 CRC32

目的：确保固件在传输与烧录过程中未被破坏。

- **算法**：`fdb_calc_crc32` = 标准 CRC-32（`init=0xFFFFFFFF`，末异或 `0xFFFFFFFF`），与 `zlib.crc32` 一致。
- **App 端（增量）**：每收到一块即 `g_ota_crc = fdb_calc_crc32(g_ota_crc, data, len)`（`ota.c:91`），收满后比对 `g_ota_crc == g_ota_exp_crc`。
- **Boot 端（全量）**：`calc_spi_crc(OTA_FW_A_OFFSET, new_len)` 校验外部 Flash；烧录后再 `calc_internal_crc(new_len)` 校验内部 Flash。两关都通过才置 `DONE`。

### 5.3 元数据与边界保护

- **magic 校验**：`ota_meta.magic == 0x4C55424F ("LUBO")` 才认定为有效升级块；否则 Boot 静默跳过 OTA。
- **接收越界保护**：`ota_feed_chunk` 中 `if (g_ota_off + len > g_ota_exp_len) return -1`，防止畸形/超长数据破坏分区（`ota.c:67`）。
- **烧录容量保护**：`burn_spi_to_internal` 中 `if (len > APP_FLASH_SIZE) return -1`，绝不越界访问不存在的内部 Flash 地址（`main_boot.c:315`）。
- **文件名强校验**：TFTP `open` 回调限定文件名与写模式，防止误写。

## 六、升级流程设计

### 6.1 App 侧接收流程（ota.c）

1. 上位机经 TCP 下发 `upgrade_start`（`new_len`, `new_crc32`）。
2. 设备 HMAC 校验通过 → `ota_handle_upgrade_start()`：
   - 拒绝重入（`g_ota_state != IDLE` 直接返回）；
   - 校验参数（长度 >0 且 ≤512KB、CRC≠0）；
   - 擦除 `firmware_a` 全分区（1MB）；
   - 记录期望 `len/crc`，置 `OTA_RX_ACTIVE`，调 `tftp_init()` 启动 TFTP Server。
3. 上位机 `put` 固件 → 设备 `tftp_write_cb` 每收到一块 → `ota_feed_chunk` 写 SPI + 增量 CRC。
4. 传输结束 `tftp_close_cb → ota_on_complete`：
   - `off==exp_len 且 crc==exp_crc` → 写 `ota_meta(PENDING)` → `HAL_NVIC_SystemReset()` 软复位；
   - 否则 `ota_reset_session()`（停 TFTP、回 `IDLE`，允许重试）。

### 6.2 Boot 侧烧录流程（main_boot.c）

上电 `main()`：最小初始化（HAL/时钟/SPI1）→ `HAL_Delay(30)` 等 W25Q64 上电稳定 → 带重试（5 次）读 `ota_meta` → 按 `state` 分支：

```
state == PENDING → do_update(&meta)
state == DONE    → 上次烧完但 App 未确认（起不来）→ 回滚 firmware_bak
state == CONFIRMED → 清为 NONE
NONE / FAILED    → 不动作
```

`do_update()` 核心顺序：

1. `calc_spi_crc(firmware_a)` 校验外部新固件完整；不符 → 写 `FAILED` 返回。
2. `backup_app_to_bak()` 把当前内部 Flash App 全量（512KB）备份到 `firmware_bak`。
3. `burn_spi_to_internal(firmware_a)`：擦内部 BANK2 + 以 32 字节 flash word 为单位编程烧录。
4. `calc_internal_crc()` 再校验内部 Flash；不符 → 用 `firmware_bak` 回滚；相符 → 写 `DONE`。
5. `write_meta()` 落盘状态。
6. `app_valid()` 校验栈顶/复位向量合法 → `JumpToApp()`（关中断、设 MSP、改 `VTOR=0x08100000`、跳复位向量）。

> 烧录全程每 32 字节 `boot_kick_iwdg()` 喂独立看门狗，避免长操作触发看门狗复位。

### 6.3 防砖确认与回滚

- **先备份再烧录**：`do_update` 先把可运行的旧 App 拷到 `firmware_bak`，再动内部 Flash。任何烧录/校验失败都用 `firmware_bak` 还原，设备不会变砖。
- **确认点机制（DONE→CONFIRMED）**：Boot 烧完只置 `DONE` 即跳 App；只有 App **真的跑起来**并调用 `ota_app_confirm()` 才置 `CONFIRMED`（`ota.c:282`）。若新固件有 bug 起不来，`ota_meta` 停留在 `DONE`，下次上电 Boot 判定"上次没确认" → 回滚 `firmware_bak`。
- **启动确认实现**：`freertos.c` 在任务启动后调用 `ota_app_confirm()`；其内部用 `static s_done` 保证每上电只确认一次，仅当 `meta.state == DONE` 时改写为 `CONFIRMED`。

### 6.4 完整时序

```
上位机                        设备 App                        设备 Boot
  |                              |                               |
  |-- TCP upgrade_start ------->|                               |
  |                              | HMAC 校验 + 擦 firmware_a    |
  |<-- upgrade_start_ack -------|                               |
  |                              | tftp_init() (UDP/69 监听)    |
  |-- TFTP WRQ ----------------->|                               |
  |<-- ACK(block0) -------------|                               |
  |-- DATA block1 ------------->| tftp_write_cb → 写 SPI+CRC   |
  |<-- ACK(block1) -------------|                               |
  | ... 逐块 ...                |                               |
  |-- 末块(<512) -------------->|                               |
  |<-- 最终 ACK ----------------|                               |
  |                              | 收满校验 → 写 meta(PENDING)  |
  |                              | HAL_NVIC_SystemReset()       |
  |                              |======== 软复位 ============>|
  |                              |                               | 读 meta(retry)
  |                              |                               | PENDING → do_update
  |                              |                               | 校验CRC→备份→烧录→校验
  |                              |                               | 置 DONE → JumpToApp
  |                              |<======== 跳转到 App =========|
  |                              | ota_app_confirm(): DONE→CONFIRMED
  |                              | 新固件运行，升级完成          |
```

## 七、关键参数与约束

| 参数 | 值 | 说明 |
|------|----|------|
| TFTP 文件名 | `H743_VS_GCC_IDE.bin` | 设备端 `OTA_TFTP_FILENAME`（`ota.h:11`） |
| TFTP 端口 | UDP 69 | 握手端口，数据走临时端口 |
| TCP 控制端口 | 50010 | `TCP_SERVER_PORT`（`net_comm_task.h:40`） |
| App 固件上限 | 512 KB | `OTA_APP_MAX_SIZE` |
| CRC32 | 标准 CRC-32 | init/末异或 0xFFFFFFFF，同 `zlib.crc32` |
| HMAC | SHA256 取前 4 字节 | 8 位大写 hex 作为 `sign` |
| HMAC 密钥 | `0123456789abcdef0123456789abcdef` | `project_config.h`，改需同步上位机 |
| Boot 等待 | 30ms + meta 读重试 5 次 | 等 W25Q64 上电稳定（`main_boot.c`） |
| 看门狗 | 每 32 字节喂一次 | 防烧录期间 IWDG 复位 |

## 八、调试与日志

### 8.1 App 侧

- 日志前缀 `LOG_OTA`，受 `OTA_DEBUG` 开关控制。
- 关键节点：升级接收开始、每块越界、收尾成功/中止、写 meta、软复位、启动确认。
- 启动即打印 `ota_meta` 当前状态：`state=`（0=NONE/1=PENDING/2=DONE/3=CONFIRMED/4=FAILED），可直接判断上次升级走到哪一步。

### 8.2 Boot 侧

- 开启 `BOOT_DEBUG_UART` 后打印 `[BOOT] ...` 全流程：
  - `magic/state/new_len/new_crc` 读取与重试；
  - `do_update` 的 CRC 比对、备份进度；
  - 烧录进度（每 64KB 打印一次）；
  - `verify` 结果、`meta` 最终状态。
- 定位"升级卡在哪一步"的主要抓手。

### 8.3 状态速查

| `ota_meta.state` | 含义 | 处理建议 |
|------------------|------|---------|
| 0 | 无待升级 | 正常 |
| 1 | 已接收待烧录 | 重启后 Boot 应自动烧录 |
| 2 | 已烧录未确认 | 若反复出现 → 新固件起不来，将自动回滚 |
| 3 | 已确认 | 升级成功闭环 |
| 4 | 失败 | 检查 CRC/固件完整性，重新发起升级 |
