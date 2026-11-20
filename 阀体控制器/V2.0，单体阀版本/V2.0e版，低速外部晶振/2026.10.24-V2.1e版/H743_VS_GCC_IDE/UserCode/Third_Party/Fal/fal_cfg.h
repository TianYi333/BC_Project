/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-05-17     armink       the first version
 */

#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

#define FAL_DEBUG 1		//启动打印
#define FAL_PART_HAS_TABLE_CFG	//启动设备表
#define FAL_USING_SFUD_PORT	//使用sfud通用串行flash

#define NOR_FLASH_DEV_NAME             "norflash0"

/* ===================== Flash device Configuration ========================= */
extern struct fal_flash_dev nor_flash0;

/* flash device table */
#define FAL_FLASH_DEV_TABLE                                          \
{                                                                    \
    &nor_flash0,                                                     \
}
/* ====================== Partition Configuration ========================== */
#ifdef FAL_PART_HAS_TABLE_CFG
/* partition table */
#define FAL_PART_TABLE                                                               \
{                                                                                    \
    {FAL_PART_MAGIC_WORD, "ef_kvdb1", NOR_FLASH_DEV_NAME,         0, 64*1024, 0}, \
    {FAL_PART_MAGIC_WORD, "ef_kvdb2", NOR_FLASH_DEV_NAME, 64*1024, 64*1024, 0}, \
    {FAL_PART_MAGIC_WORD, "ef_kvdb3", NOR_FLASH_DEV_NAME, 128*1024, 64*1024, 0}, \
    /* ---- OTA 远程升级分区（SPI Flash 总容量 8MB，以下均在范围内） ---- */ \
    {FAL_PART_MAGIC_WORD, "firmware_a",  NOR_FLASH_DEV_NAME, 192*1024, 1024*1024, 0}, \
    {FAL_PART_MAGIC_WORD, "firmware_bak",NOR_FLASH_DEV_NAME,1216*1024, 1024*1024, 0}, \
    {FAL_PART_MAGIC_WORD, "ota_meta",    NOR_FLASH_DEV_NAME,2240*1024,   64*1024, 0}, \
}

#endif /* FAL_PART_HAS_TABLE_CFG */

#endif /* _FAL_CFG_H_ */
