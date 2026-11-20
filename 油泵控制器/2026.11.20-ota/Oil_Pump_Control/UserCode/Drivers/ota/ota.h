#ifndef OTA_APP_H
#define OTA_APP_H

#include <stdint.h>
#include "ota_common.h"

/* OTA 固件传输方式：当前采用 lwIP TFTP server（UDP/69，量产路径）。
 * TFTP 接收文件名由本次 upgrade 命令下发的 filename 决定（落库到 g_ota_exp_fname），
 * 不再使用固定宏；OTA_TFTP_FILENAME 仅作网关侧默认参考。 */
#ifndef OTA_TFTP_FILENAME
//#define OTA_TFTP_FILENAME "firmware.bin"
#define OTA_TFTP_FILENAME "Oil_Pump_Control.bin"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* App 端 OTA 接收状态机 */
typedef enum {
    OTA_RX_IDLE   = 0,  /* 无升级在进行 */
    OTA_RX_ACTIVE,      /* 已收到 upgrade，TFTP 接收中 */
    OTA_RX_DONE         /* 接收完成且校验通过，已写 meta 并触发重启（不会长期停留） */
} ota_rx_state_t;

/**
 * 由 net_comm_task.c 的 upgrade 命令调用：
 *   校验参数 → 擦 firmware_a → 记录期望 len/crc/filename → 启动 TFTP 接收服务。
 * @param new_len     期望固件字节数
 * @param new_crc32   期望固件 CRC32（与 fdb_calc_crc32 算法一致）
 * @param filename    期望 TFTP 接收文件名（来自命令，须与网关推送文件名一致，≤ OTA_FILENAME_MAX_LEN）
 * @return 0 成功接纳；<0 失败：
 *         -1 忙碌 / -2 长度非法 / -3 CRC 非法 / -6 文件名非法 /
 *         -4 分区缺失 / -5 TFTP 初始化失败
 */
int ota_handle_upgrade_start(uint32_t new_len, uint32_t new_crc32,
                             const char *filename);

/* OTA 状态/结果上报回调（由 net_comm_task.c 实现，供本模块在升级关键节点调用） */
void tcp_report_upgrade_status(const char *status_str, const char *message);
void tcp_report_upgrade_result(uint8_t result, const char *message);
void tcp_store_ota_old_version(const char *ver);

/**
 * App 启动确认（防砖确认点）：把 ota_meta 由 DONE 改写为 CONFIRMED。
 * 内部静态标志保证每上电只执行一次；仅当 meta.state == DONE 时才真正写。
 * 应在 MainLogicTask 第一次成功跑完一轮控制循环后调用。
 */
void ota_app_confirm(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_APP_H */
