/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-05-17     armink       the first version
 * 2026-09-07     port         switch to STM32F767 on-chip Flash (replaces SPI/W25Q64)
 */

#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

/* 静音 FAL 上电启动日志：
 *   FAL_DEBUG 0    -> 关闭 [D/FAL]（Flash device 注册信息）
 *   FAL_LOG_INFO 0 -> 关闭 [I/FAL]（分区表 + "initialize success"）
 * [E/FAL] 错误日志始终保留，便于定位 Flash 写入故障（如 Partition write error）。 */
#define FAL_DEBUG 0
#define FAL_LOG_INFO 0
#define FAL_PART_HAS_TABLE_CFG	//启动设备表

/* On-chip STM32F767 internal Flash port (replaces the previous
 * FAL_USING_SFUD_PORT SPI/W25Q64 driver). */
#define FAL_USING_STM32_ONCHIP_FLASH_PORT

#define STM32_ONCHIP_FLASH_DEV_NAME     "stm32_onchip"

/* ===================== Flash device Configuration ========================= */
extern struct fal_flash_dev stm32_onchip;

/* flash device table */
#define FAL_FLASH_DEV_TABLE                                          \
{                                                                    \
    &stm32_onchip,                                                   \
}

/* ====================== Partition Configuration ========================== */
#ifdef FAL_PART_HAS_TABLE_CFG
/* partition table
 *
 * ⚠️ F767ZGT6 = 1MB Flash（G=1MB，不是 2MB）。双 Bank（nDBANK=0）：
 *   Bank1 = 512KB @0x08000000（S0~S3=16K, S4=64K, S5~S7=128K）
 *   Bank2 = 512KB @0x08080000（S12~S15=16K, S16=64K, S17~S19=128K）
 * 扇区号不连续（AN4826 图1）；Bank2 的 SNB 寄存器值 = 扇区号 + 4。
 *   ef_kvdb1: 0x08080000 (S12~S15) = 64KB，4 个 16KB 扇区（满足 FlashDB ≥2 扇区磨损均衡）。
 * App 在 Bank2：写 KV/meta（Bank2）为同 Bank -> 经 .RamFunc SRAM 驻留原语安全完成；
 * 写 firmware_a（Bank1）为跨 Bank（RWW，不影响运行）。
 *
 * The OTA firmware partitions (firmware_a / firmware_bak / ota_meta) that used
 * to live on the 8MB external W25Q64 have been removed; OTA is temporarily
 * disabled (ota.c will simply not find those partitions at runtime). */
/* partition table（全部落在 STM32F767 内部 Flash，1MB 双 Bank 非均匀扇区）
 *
 * 设备 stm32_onchip 覆盖 0x08020000~0x080FFFFF（896KB = Bank1 S5~S7 + Bank2 S12~S19）：
 *   firmware_a : 0x08020000 (S5~S7)   384KB  —— TFTP 接收新固件的暂存区
 *   ef_kvdb1   : 0x08080000 (S12~S15)  64KB  —— 配置 KV（4x16KB 磨损均衡）
 *   ota_meta   : 0x08090000 (S16)      64KB  —— OTA 状态字（magic/state/len/crc）
 *
 * App 已重定位到 Bank2 0x080A0000（见 STM32F767xx_FLASH.ld）。
 * 各分区 offset 为相对 stm32_onchip.addr(0x08020000) 的偏移，须与 fal_flash
 * 驱动 g_sec[] 的物理扇区映射严格一致。 */
#define FAL_PART_TABLE                                                                                       \
{                                                                                                            \
    {FAL_PART_MAGIC_WORD, "firmware_a", STM32_ONCHIP_FLASH_DEV_NAME, 0,        384*1024, 0},                 \
    {FAL_PART_MAGIC_WORD, "ef_kvdb1",  STM32_ONCHIP_FLASH_DEV_NAME, 384*1024,  64*1024, 0},                  \
    {FAL_PART_MAGIC_WORD, "ota_meta",   STM32_ONCHIP_FLASH_DEV_NAME, 448*1024,  64*1024, 0},                 \
}
#endif /* FAL_PART_HAS_TABLE_CFG */

#endif /* _FAL_CFG_H_ */
