# 项目长期记忆

## 项目关系
- 当前项目 `H743_VS_GCC_IDE`（低速内部晶振版，`d:\Desktop\temporary\2026.10.08-V2.0版，低速内部晶振\`）是参考项目 `d:\Desktop\H743_VS_GCC_IDE` 的低速时钟源变体。
- 两项目唯一功能差异：低速时钟源相关文件 `Core/Src/{gpio.c,main.c,rtc.c}` 与 `H743_VS_GCC_IDE.ioc`。其余（HAL 库、CMake 配置、OTA 逻辑）应保持一致。
- 跨项目移植时：只动目标差异文件，时钟文件切勿改动。

## OTA 实现（2026-10-12 整体同步至参考项目）
- OTA 采用「丰富版」协议：命令 `upgrade`（`crc` hex串 / `filename` / `file_size`），`ota_handle_upgrade_start(uint32_t len, uint32_t crc, const char* filename)` 三参；TFTP 文件名由命令下发的 filename 决定（`g_ota_exp_fname`）。
- 含 `upgrade_status` / `upgrade_result` 上报（`net_comm_task.c` 实现）；`upgrade_result` 未连网时缓存到 FlashDB（key `ota_result`），连上后由 `upgrade_result_check_and_report()` 补发；旧版本号存 `ota_oldver`。
- 协议常量 `OTA_FILENAME_MAX_LEN=20`、`OTA_TFTP_PORT=69`（见 `ota_common.h`）。
- 注意：协议再次变更时，须同步上位机/推送脚本（`ota_tftp_push.py` 等）。

## 构建注意
- `build/Debug` 的 CMakeCache 指向参考项目路径，不可直接用于验证当前项目；验证当前项目须用全新 build 目录（如 `build/verify`）并配置 `cmake --preset Debug -B build/verify`，且 PATH 含 `arm-none-eabi-`（如 `/d/APP/GCC-ARM/bin`）。
