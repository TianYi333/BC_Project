# OTA 升级协议与流程说明文档

> 本文档依据 **当前工程代码**（`d:\Desktop\H743_VS_GCC_IDE`）逆向梳理，反映 2026-10-12 实际实现状态。
> 适用项目：H743_VS_GCC_IDE（控制节点固件，STM32H743ZGT6 + FreeRTOS + lwIP + FlashDB + 外部 W25Q64，设备型号 LUB-CTRL-V2.0）。
> 主要源码：`UserCode/Drivers/ota/`（OTA 驱动）、`UserCode/Drivers/ETH/net_comm_task.c`（TCP/TFTP 命令与上报）、`d:\Desktop\OTA_Upgrade_Tool\ota_tftp_push.py`（网关推送脚本）。

---

## 1. 总体架构

| 项 | 说明 |
|----|------|
| MCU | STM32H743ZGT6（Cortex-M7，双 Bank Flash） |
| 软件栈 | FreeRTOS + lwIP（TCP/UDP）+ FlashDB（KV 库） |
| 外部存储 | W25Q64（SPI NOR，8MB），分区：`firmware_a@0x30000`、`firmware_bak@0x130000`、`ota_meta@0x230000` |
| 双 Bank 防砖 | Bootloader @ `0x08000000` / App @ `0x08100000`；升级即把 `firmware_a` 烧录进 App 区 |
| OTA 状态机 | `NONE=0` / `PENDING=1` / `DONE=2` / `CONFIRMED=3` / `FAILED=4`，magic = `0x4C55424F` |
| 固件校验 | 标准 CRC-32（`fdb_calc_crc32`，IEEE 802.3/PKZIP，init=0xFFFFFFFF，final xor=0xFFFFFFFF），与 `zlib.crc32` 等价 |

---

## 2. 网络通道与端口

| 通道 | 协议 / 端口 | 用途 |
|------|------------|------|
| 控制通道 | TCP `50010` | 网关下发 `upgrade` 命令，设备回复 `upgrade_ack` / `upgrade_status` / `upgrade_result` |
| 文件通道 | TFTP（UDP `69`） | 网关把固件 `.bin` 推送到设备 TFTP Server |

> 设备 TFTP Server 监听端口固定为 UDP `69`（由 lwIP `tftp_opts.h` 的 `TFTP_PORT` 决定）；`upgrade` 命令不再携带 TFTP 端口 / Server / 超时字段。

---

## 3. 安全机制（HMAC-SHA256 验签）

- 所有上行/下行 JSON 帧均带 `sign` 字段。
- 签名算法：`HMAC-SHA256(key=HMAC_KEY, msg=签名源串)`，取摘要**前 4 字节**按**大写 HEX** 拼接为 8 字符 `sign`。
- `HMAC_KEY`：定义于 `UserCode/Logic/project_config.h`（`#define HMAC_KEY "0123456789abcdef0123456789abcdef"`），网关脚本 `--hmac-key` 须与之完全一致。
- 签名源串 = **不含 `sign` 字段的 JSON**（字段严格字典序），设备与网关各自构造并比对。
- `ts` 字段为 Unix 时间的字符串（如 `"1728000000"`）。

---

## 4. 协议帧总览

| 帧 | 方向 | type 值 | 是否带 sign | 说明 |
|----|------|---------|------------|------|
| `upgrade` | 网关 → 设备 | `upgrade` | 是 | 下发升级参数（文件名/大小/CRC） |
| `upgrade_ack` | 设备 → 网关 | `upgrade_ack` | 是 | 接受或拒绝升级（含错误码） |
| `upgrade_status` | 设备 → 网关 | `upgrade_status` | 是 | 过程状态（接收完成/校验失败/重启） |
| `upgrade_result` | 设备 → 网关 | `upgrade_result` | 是 | 升级结果（成功确认后上报） |

---

## 5. 帧字段详述

### 5.1 `upgrade`（网关 → 设备）

下发升级指令，触发设备进入接收态。

**字段**

| 字段 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 设备唯一 ID |
| `crc` | string | 固件 CRC-32 的 **8 位大写 HEX**（如 `"1A2B3C4D"`），设备端按 16 进制解析为 `uint32` |
| `file_size` | number | 固件字节数（≤ 512KB） |
| `filename` | string | 期望接收文件名，长度 **1..20** 字符 |
| `seq` | number | 命令序号 |
| `ts` | string | Unix 时间字符串 |
| `type` | string | 固定 `"upgrade"` |
| `sign` | string | 4 字节 HMAC 前缀，大写 HEX |

**HMAC 签名源串（设备端 `tcp_sign_buf_2048`，与网关 `build_sign_src` 一致）**

```
{"crc":"%s","device_id":"%s","file_size":%lu,"filename":"%s","seq":%lu,"ts":%s,"type":"upgrade"}
```

**设备端处理（`net_comm_task.c` 解析 + `ota_handle_upgrade_start`）**

1. 校验 `sign`（HMAC-SHA256 比对）。
2. 解析字段；`crc` 用 `strtoul(..., 16)` 解析为 `uint32`。
3. 调用 `ota_handle_upgrade_start(new_len, new_crc32, filename)`，按返回码回复 `upgrade_ack`（见 §6 错误码）。

### 5.2 `upgrade_ack`（设备 → 网关）

设备对 `upgrade` 命令的响应。

**字段**

| 字段 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 设备 ID |
| `estimated_duration` | number | 预计耗时（秒）；接受时填 `60`，拒绝时填 `0` |
| `message` | string | `"upgrade_accepted"` / `"upgrade_rejected"` |
| `seq` | number | 序号 |
| `status` | number | **错误码**（见 §6） |
| `ts` | number | Unix 时间 |
| `type` | string | 固定 `"upgrade_ack"` |
| `sign` | string | HMAC 前缀 |

**HMAC 签名源串**

```
{"device_id":"%s","estimated_duration":%u,"message":"%s","seq":%lu,"status":%d,"ts":%lu,"type":"upgrade_ack"}
```

> 网关脚本以 `ACK_KEYWORD = b'upgrade_ack'` 等待该帧。

### 5.3 `upgrade_status`（设备 → 网关）

升级过程中的过程状态上报（TCP 已连接时直发，未连接则丢弃）。

**字段**

| 字段 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 设备 ID |
| `message` | string | 描述信息 |
| `seq` | number | 序号 |
| `status` | string | 取值见下表 |
| `ts` | number | Unix 时间 |
| `type` | string | 固定 `"upgrade_status"` |
| `sign` | string | HMAC 前缀 |

**`status` 取值**

| 值 | 触发时机 |
|----|---------|
| `recv_done` | TFTP 文件接收完成，进入 CRC 校验阶段 |
| `verify_fail` | CRC-32 / 长度校验失败（升级失败终止，不重启） |
| `reboot` | 校验通过，即将软复位以应用新固件 |

**HMAC 签名源串**

```
{"device_id":"%s","message":"%s","seq":%lu,"status":"%s","ts":%lu,"type":"upgrade_status"}
```

### 5.4 `upgrade_result`（设备 → 网关）

升级成功确认后的最终结果上报。

> 本帧**已移除** `error_code`、`error_msg`、`reboot_required` 三个字段（2026-10-12 调整），仅保留下列字段。

**字段**

| 字段 | 类型 | 说明 |
|------|------|------|
| `device_id` | string | 设备 ID |
| `message` | string | 如 `"upgrade_success"` |
| `model` | string | `DEVICE_MODEL`（当前 `"LUB-CTRL-V2.0"`） |
| `new_version` | string | 新固件版本号（见 §7 版本号约定） |
| `old_version` | string | 升级前版本号（见 §7 版本号约定） |
| `result` | number | `1` = 成功 |
| `seq` | number | 序号 |
| `ts` | number | Unix 时间 |
| `type` | string | 固定 `"upgrade_result"` |
| `sign` | string | HMAC 前缀 |

**HMAC 签名源串**

```
{"device_id":"%s","message":"%s","model":"%s","new_version":"%s","old_version":"%s","result":%d,"seq":%lu,"ts":%lu,"type":"upgrade_result"}
```

> 若上报时 TCP 尚未连上，结果缓存到 `reboot_kvdb`（key `ota_result`），待 TCP 连接成功后由 `upgrade_result_check_and_report()` 补发（见 §8）。

---

## 6. `upgrade_ack.status` 错误码映射

`status = (ota_ret == 0) ? 0 : (uint8_t)(-ota_ret)`，含义如下：

| status | 含义 | 对应 `ota_handle_upgrade_start` 返回值 |
|--------|------|----------------------------------------|
| `0` | 接受，已开始监听 TFTP | 成功（返回 0） |
| `1` | 设备忙碌（正在升级中，拒绝新请求） | `-1` |
| `2` | 固件长度非法（`0` 或超过 512KB） | `-2` |
| `3` | CRC 校验值非法（CRC = 0） | `-3` |
| `4` | 分区缺失（`firmware_a` / `ota_meta` 未找到） | `-4` |
| `5` | TFTP 服务初始化失败 | `-5` |
| `6` | 文件名非法（空或超过 20 字符） | `-6` |

**校验顺序**（在 `ota_handle_upgrade_start` 内依次执行）：`-1` → `-2` → `-3` → `-6` → `-4` → `-5`。

---

## 7. 版本号约定

- **`old_version`（升级前版本）**：在收到 `upgrade` 命令、校验通过时，把当前运行固件的 `DEVICE_FW_VER` 冻结并存入 FlashDB（`reboot_kvdb`，key `ota_oldver`）；重启烧录新固件后仍能读回，作为 `upgrade_result.old_version`。
- **`new_version`（新固件版本）**：上报时**直接读取**当前运行固件的 `DEVICE_FW_VER` 宏（不落盘）。因上报发生在新固件启动后，此时读到的是新固件的版本号。
- **约束**：每次出新固件，必须手动将 `project_config.h` 的 `DEVICE_FW_VER` 改为新版本号后重新编译；协议本身不校验"新>旧"。

---

## 8. 防砖与掉电恢复机制

### 8.1 OTA 状态机（`ota_meta`）

| 状态 | 写入方 | 含义 |
|------|--------|------|
| `NONE` | 初始 | 无升级 |
| `PENDING` | OTA 驱动（收满校验通过、复位前） | 已收固件待烧录 |
| `DONE` | Bootloader（烧录完成时） | 新固件已写入 App 区 |
| `CONFIRMED` | App 启动确认（`ota_app_confirm`） | 新固件启动成功，防砖确认点 |
| `FAILED` | 异常 | 升级失败 |

> 防砖逻辑：App 启动约 5 秒后 `ota_app_confirm()` 检查 `ota_meta.state == DONE` 才置 `CONFIRMED`；若未确认（启动失败），状态停留在 `PENDING`/`FAILED`，下次上电 Boot 可回退，避免变砖。

### 8.2 `reboot_kvdb` 持久化内容

独立 KV 库（库名 `rebootdb`，分区 `ef_kvdb3`），专用于跨重启传递数据：

| key | 类型 | 内容 | 生命周期 |
|-----|------|------|---------|
| `reboot_info` | blob | 主动重启原因（`RebootPersistentInfo_t`，magic `0x52454254`） | 上电上报后删除（一次性） |
| `ota_oldver` | string | 升级前版本号 | 升级结果上报后由新固件逻辑不再依赖，可保留 |
| `ota_result` | blob | 升级结果缓存（`ota_result_blob_t`，magic `0x4F544152`） | TCP 连上补发后删除 |

### 8.3 `upgrade_result` 缓存补发

`tcp_report_upgrade_result()` 在 `ota_app_confirm()` 中调用：若当时 `g_tcp_connected != 1`，先把结果 blob 存入 `ota_result`；TCP 连接成功分支调用 `upgrade_result_check_and_report()` 读出并发送、随后删除。保证重启后即便 TCP 延迟连上，结果不丢失。

---

## 9. 完整升级时序流程

```
网关                                  设备(TCP 50010 / TFTP UDP 69)
 │                                      │
 │── 1. upgrade 命令(HMAC sign) ───────>│  校验 sign / 解析字段 / 合法性校验
 │                                      │  └─ ota_handle_upgrade_start()
 │<── 2. upgrade_ack(status) ───────────│  接受(status=0) 或 拒绝(status=1..7)
 │                                      │
 │   (拒绝则流程结束)                    │
 │                                      │  擦 firmware_a → 启动 TFTP Server(69)
 │                                      │
 │── 3. TFTP PUT .bin (filename 匹配) ─>│  增量接收 + CRC-32 累计
 │                                      │
 │                           收满 ──────┼─> 比对 CRC/长度
 │                                      │     ├─ 不符: upgrade_status(verify_fail) → 终止(不重启)
 │                                      │     └─ 相符:
 │<── 4. upgrade_status(recv_done) ─────│
 │<── 5. upgrade_status(reboot) ────────│  写 ota_meta=PENDING → 软复位
 │                                      │
 │                      Boot 烧录 ──────┼─> firmware_a → App 区(0x08100000)
 │                                      │     ota_meta: PENDING → DONE
 │                      新 App 启动 ────┼─> ~5s 后 ota_app_confirm()
 │                                      │     DONE → CONFIRMED
 │<── 6. upgrade_result(result=1) ──────│  (TCP 未连则缓存，连上补发)
 │                                      │
```

**关键步骤说明**

1. 网关下发 `upgrade`（带 HMAC `sign`）；设备校验签名与字段合法性。
2. 设备回复 `upgrade_ack`，`status` 编码接受/拒绝原因（§6）。
3. 接受后设备擦 `firmware_a` 并启动 TFTP Server，仅接受 `filename` 匹配的文件。
4. 网关用 TFTP 推送 `.bin`。
5. 收满后做 CRC-32 比对：不匹配上报 `verify_fail` 并终止；匹配上报 `recv_done` → 写 `ota_meta=PENDING` → 上报 `reboot` → 软复位。
6. Bootloader 将 `firmware_a` 烧录进 App 区，状态推进为 `DONE`，跳转 App。
7. 新 App 启动约 5 秒后 `ota_app_confirm()`：状态 `DONE → CONFIRMED`（防砖确认），上报 `upgrade_result`。

---

## 10. 网关工具 `ota_tftp_push.py` 使用

路径：`d:\Desktop\OTA_Upgrade_Tool\ota_tftp_push.py`

**基本用法**

```
python ota_tftp_push.py <firmware.bin> --device-id <DEVICE_ID> ^
       [--port 50010] [--seq N] ^
       [--tftp-host <设备IP>] ^
       [--hmac-key <HMAC_KEY>] [--tftp-filename <NAME>]
```

**关键参数**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `bin`（位置参数） | — | 待升级 `.bin` 文件路径 |
| `--device-id` | 必填 | 设备 DEVICE_ID |
| `--port` | `50010` | TCP 控制通道监听端口 |
| `--tftp-filename` | `H743_VS_GCC_IDE.bin` | 同时作为命令 `filename` 与 TFTP put 文件名，须 ≤ 20 字符 |
| `--hmac-key` | `HMAC_KEY` | HMAC 密钥，须与 `project_config.h` 一致 |

**行为**

- 计算 `crc = zlib.crc32(data) & 0xFFFFFFFF`，格式化为 8 位大写 HEX（`"%08X"`）。
- 发送 `upgrade` 命令（type=`upgrade`，带 HMAC `sign`），等待 `upgrade_ack`（`ACK_KEYWORD = b'upgrade_ack'`）。
- 收到确认后，用 TFTP 把 `.bin` 推送到 `tftp_host`（UDP 69），文件名 = `--tftp-filename`。

---

## 11. 关键代码位置索引

| 内容 | 位置 |
|------|------|
| 协议常量 `OTA_FILENAME_MAX_LEN=20` / `OTA_TFTP_PORT=69` | `UserCode/Drivers/ota/ota_common.h` |
| `ota_handle_upgrade_start` 原型 | `UserCode/Drivers/ota/ota.h` |
| 校验逻辑（返回码 -1~-6） | `UserCode/Drivers/ota/ota.c`（`ota_handle_upgrade_start`） |
| TFTP 接收回调 / 期望文件名比对 | `UserCode/Drivers/ota/ota.c`（`tftp_open_cb` / `tftp_write_cb`） |
| `recv_done` / `verify_fail` / `reboot` 上报 | `UserCode/Drivers/ota/ota.c`（`ota_on_complete`） |
| `upgrade_result` 上报（成功确认） | `UserCode/Drivers/ota/ota.c`（`ota_app_confirm`） |
| `upgrade` 命令解析 + HMAC 校验 | `UserCode/Drivers/ETH/net_comm_task.c`（解析分支） |
| `upgrade_ack` 发送 | `UserCode/Drivers/ETH/net_comm_task.c`（`tcp_send_upgrade_ack`） |
| `upgrade_status` / `upgrade_result` 发送 | `UserCode/Drivers/ETH/net_comm_task.c`（`tcp_report_upgrade_status` / `send_upgrade_result_now`） |
| `reboot_kvdb` 初始化与 KV key | `UserCode/Drivers/ETH/net_comm_task.c`（`init_reboot_db` / `KV_KEY_OTA_OLDVER` / `KV_KEY_OTA_RESULT`） |
| HMAC 密钥 / 设备型号 / 版本宏 | `UserCode/Logic/project_config.h`（`HMAC_KEY` / `DEVICE_MODEL` / `DEVICE_FW_VER`） |
| 网关推送脚本 | `d:\Desktop\OTA_Upgrade_Tool\ota_tftp_push.py` |

---

## 12. 注意事项与已知约束

1. **版本号无自动校验**：`new_version` 纯靠编译期 `DEVICE_FW_VER`；若新固件沿用旧版本号，网关会看到 `old_version == new_version`。
2. **文件名 ≤ 20 字符**：受 lwIP `TFTP_MAX_FILENAME_LEN` 限制；`upgrade` 的 `filename` 必须与之匹配。
3. **`crc` 为 HEX 字符串**：设备端按 16 进制解析；值为 `0` 视为非法（错误码 3）。
4. **本环境未做 ARM 编译验证**：文档基于源码静态梳理，工程须在 STM32CubeIDE 重新编译确认。
6. **`.docx` 自动转换不可用**：本文档以 Markdown 交付（本机 Word 转换引擎依赖缺失/无外网）；如需 `.docx`，可手动用 Word 打开本 `.md` 另存，或在本机配置 Python 依赖后重跑转换。
