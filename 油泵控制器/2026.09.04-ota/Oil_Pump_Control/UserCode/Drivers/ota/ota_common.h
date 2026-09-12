#ifndef OTA_COMMON_H
#define OTA_COMMON_H

#include <stdint.h>

/* OTA meta 魔数 "LUBO" */
#define OTA_META_MAGIC        0x4C55424FUL

/* OTA 状态机（Bootloader 与 App 端 ota.c 共用同一套语义） */
#define OTA_STATE_NONE        0   /* 无待升级 */
#define OTA_STATE_PENDING     1   /* firmware_a 已就绪，待 Bootloader 烧录 */
#define OTA_STATE_DONE        2   /* Bootloader 已烧录，待 App 启动确认 */
#define OTA_STATE_CONFIRMED   3   /* App 启动成功，已确认（升级完成） */
#define OTA_STATE_FAILED      4   /* 烧录/校验失败 */

/* 物理偏移：与 fal_cfg.h 的 FAL_PART_TABLE 严格对齐（W25Q64, 8MB） */
#define OTA_FW_A_OFFSET       0x00030000UL   /* firmware_a  @ 192KB  */
#define OTA_FW_BAK_OFFSET     0x00130000UL   /* firmware_bak @ 1216KB */
#define OTA_META_OFFSET       0x00230000UL   /* ota_meta    @ 2240KB */

/* 分区大小（须与 fal_cfg.h 一致） */
#define OTA_FW_PART_SIZE      0x00100000UL   /* firmware_a/bak 各 1MB */
#define OTA_META_SIZE         0x00010000UL   /* ota_meta 64KB */

/* App 内部 Flash 区（与 STM32H743XG_FLASH.ld 的 FLASH ORIGIN 一致） */
#define OTA_APP_ADDRESS       0x08100000UL
#define OTA_APP_MAX_SIZE      0x00080000UL   /* bank2 共 512KB（H743ZGT6 双bank各512KB） */

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t new_len;     /* 新固件字节数（<= OTA_APP_MAX_SIZE，即内部Flash bank2 512KB） */
    uint32_t new_crc32;   /* 新固件 CRC32（与 fdb_calc_crc32 算法一致） */
    uint32_t reserved[4];
} ota_meta_t;             /* 32 字节 */

#endif /* OTA_COMMON_H */
