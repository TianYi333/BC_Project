# 项目长期记忆（H743_VS_GCC_IDE）

## 工程路径（权威，2026-10-12 由用户确认）
- **App 工程（控制节点固件）**：`/d/Desktop/H743_VS_GCC_IDE`（STM32H743ZGT6 + FreeRTOS + lwIP + FlashDB + 外部 W25Q64，设备型号 `LUB-CTRL-V2.0`，固件名 `H743_VS_GCC_IDE.bin`）。
- **Boot 工程（两项目共用）**：`/d/Desktop/boot`（原 `Oil_Pump_Control_Boot` 重命名；Boot@0x08000000，App@0x08100000，双 Bank 防砖）。**`H743_VS_GCC_IDE` 与 `Oil_Pump_Control` 两个 App 项目共用同一份 Boot 工程**，Boot 的 `ota_common.h` 须与两 App 的 OTA 契约保持一致（当前已核对一致）。
- ⚠️ **`Oil_Pump_Control` 是另一个独立项目（油泵控制器），不是本项目的旧副本/残目录**：它与本项目（`H743_VS_GCC_IDE`，控制节点固件）并列，二者**仅 OTA 升级驱动代码相同（同源复用）**，其余固件彼此独立、互不影响。请勿把 `Oil_Pump_Control` 当残副本删除。两项目**共用同一份 Boot 工程**（`/d/Desktop/boot`）。云端画像记忆曾误把「App 工程路径」写成 `/d/Desktop/Oil_Pump_Control`，那是另一项目，不是本项目的旧名。
- 后续本项目的 App 一律以 `H743_VS_GCC_IDE` 为准；涉及 OTA 的改动两个项目需各自同步（因 OTA 代码同源）。
- OTA 驱动统一在 `UserCode/Drivers/ota`（`ota_common.h` / `ota.h` / `ota.c`），App 与 Boot 共用同一份 `ota_common.h` 语义（ota_meta_t、魔数、偏移、状态机必须保持一致）。

## OTA 协议现状（2026-10-12 最终实现）
- 四帧（App↔网关 TCP/JSON）：`upgrade` / `upgrade_ack` / `upgrade_status` / `upgrade_result`。
- `upgrade` 字段：`crc / device_id / file_size / filename / seq / ts / type`（无 tftp_port/tftp_server/timeout）。
- `upgrade_result` 字段已精简（无 error_code/error_msg/reboot_required）；old_version 存 KV，new_version 读 `DEVICE_FW_VER` 宏。
- 设备 TFTP Server 固定 UDP/69（lwIP tftp_opts.h），`filename` 仅作 App 侧门禁校验，Boot 不感知文件名。

## 工具链 / 环境限制
- 本机无 ARM 工具链，代码改动需用户在 STM32CubeIDE 重新编译 App+Boot 并硬件验证。
- 文档转换：`python-docx` 在托管 venv（`binaries/python/envs/default`）可用；`output/convert_ota_docx.py` 可重复生成 docx。

## 用户协作约定（跨项目）
- 修改项目代码/配置/文档前**先确认方案**，获批再动手；纯只读分析可先行。
- 偏好"手术刀式"最小编辑，反对误删已有字段。
