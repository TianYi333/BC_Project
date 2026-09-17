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

/* 物理地址：与 fal_cfg.h 的 FAL_PART_TABLE、STM32F767xx_FLASH.ld 严格对齐。
 * ⚠️ F767ZGT6 = 1MB Flash（G=1MB，不是 2MB！）。双 Bank（nDBANK=0）：
 *   Bank1 = 512KB @0x08000000（S0~S3=16K, S4=64K, S5~S7=128K）
 *   Bank2 = 512KB @0x08080000（S12~S15=16K, S16=64K, S17~S19=128K）
 * 扇区号不连续（AN4826 图1：双 Bank 共 16 个扇区）；Bank2 的 FLASH_CR.SNB
 * 寄存器值 = 扇区号 + 4（HAL FLASH_Erase_Sector 的 "+4" 规则）-> 16~23。
 *   - Bootloader      : Bank1 S0~S4   0x08000000 128KB（复位入口，永不改）
 *   - firmware_a(暂存): Bank1 S5~S7   0x08020000 384KB（TFTP 收新固件）
 *   - ef_kvdb1(KV)    : Bank2 S12~S15 0x08080000  64KB（4x16KB 磨损均衡）
 *   - ota_meta        : Bank2 S16     0x08090000  64KB
 *   - App(重定位后)   : Bank2 S17~S19 0x080A0000 384KB
 * 内部 Flash 仅 1MB，无法完整备份 App，故省去 firmware_bak（防砖靠 CRC+向量表校验）。 */
#define OTA_FW_A_OFFSET       0x08020000UL   /* firmware_a  暂存 @ Bank1 0x08020000 */
#define OTA_META_OFFSET       0x08090000UL   /* ota_meta     @ Bank2 0x08090000 */

/* 分区大小（须与 fal_cfg.h 一致；最小擦除粒度 16KB，物理扇区非均匀） */
#define OTA_FW_PART_SIZE      0x00060000UL   /* firmware_a 384KB */
#define OTA_META_SIZE         0x00010000UL   /* ota_meta    64KB */

/* App 内部 Flash 区（与 STM32F767xx_FLASH.ld 的 FLASH ORIGIN 一致，Bank2） */
#define OTA_APP_ADDRESS       0x080A0000UL
#define OTA_APP_MAX_SIZE      0x00060000UL   /* App 区 384KB（Bank2 S17~S19） */

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t new_len;     /* 新固件字节数（<= OTA_FW_PART_SIZE） */
    uint32_t new_crc32;   /* 新固件 CRC32（与 fdb_calc_crc32 算法一致） */
    uint32_t reserved[4];
} ota_meta_t;             /* 32 字节 */

#endif /* OTA_COMMON_H */
