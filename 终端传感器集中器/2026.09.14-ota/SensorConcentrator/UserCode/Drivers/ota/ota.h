#ifndef OTA_APP_H
#define OTA_APP_H

#include <stdint.h>
#include "ota_common.h"

/* OTA 固件传输方式：当前采用 lwIP TFTP server（UDP/69，量产路径）。
 * TFTP 接收文件名由 OTA_TFTP_FILENAME 宏定义，可在此按需修改。
 * ⚠️ 文件名必须 ≤ TFTP_MAX_FILENAME_LEN(默认20) 字符，否则 lwIP tftp_server.c
 * 会回 ERROR 2 "Filename too long/not NULL terminated"（"SensorConcentrator.bin"
 * 有 22 字符，实测被拒，故用 14 字符的短名）。 */
#ifndef OTA_TFTP_FILENAME
//#define OTA_TFTP_FILENAME "firmware.bin"
#define OTA_TFTP_FILENAME "SensorConc.bin"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* App 端 OTA 接收状态机 */
typedef enum {
    OTA_RX_IDLE   = 0,  /* 无升级在进行 */
    OTA_RX_ACTIVE,      /* 已收到 upgrade_start，TFTP/TCP 接收中 */
    OTA_RX_DONE         /* 接收完成且校验通过，已写 meta 并触发重启（不会长期停留） */
} ota_rx_state_t;

/**
 * 由 net_comm_task.c 的 upgrade_start 命令调用：
 *   校验参数 → 擦 firmware_a → 记录期望 len/crc → 启动 TFTP 接收服务。
 * @return 0 成功接纳(已开始监听)；<0 失败(忙碌 / 参数非法 / 分区缺失 / 传输初始化失败)
 */
int ota_handle_upgrade_start(uint32_t new_len, uint32_t new_crc32);

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
