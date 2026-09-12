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

#define FAL_DEBUG 1		//启动打印
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
 * KVDB 分区落在双 Bank 模式下的 Bank2 均匀 128KB 扇区：
 *   ef_kvdb1: 0x080A0000 (S17) + 0x080C0000 (S18) = 256KB，2 个扇区（满足
 *   FlashDB 至少 2 扇区磨损均衡要求）。用户数据约 64KB，余量充足。
 * 代码仍在 Bank1，写 Bank2 时 Bank1 可取指，安全（规避同 Bank 写崩溃）。
 *
 * The OTA firmware partitions (firmware_a / firmware_bak / ota_meta) that used
 * to live on the 8MB external W25Q64 have been removed; OTA is temporarily
 * disabled (ota.c will simply not find those partitions at runtime). */
#define FAL_PART_TABLE                                                               \
{                                                                                    \
    {FAL_PART_MAGIC_WORD, "ef_kvdb1", STM32_ONCHIP_FLASH_DEV_NAME, 0, 256*1024, 0}, \
}
#endif /* FAL_PART_HAS_TABLE_CFG */

#endif /* _FAL_CFG_H_ */
