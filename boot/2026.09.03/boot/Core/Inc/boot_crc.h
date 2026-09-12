#ifndef BOOT_CRC_H
#define BOOT_CRC_H

#include <stdint.h>
#include <stddef.h>

/* 与 FlashDB 的 fdb_calc_crc32 算法完全一致：
   标准 CRC-32（多项式 0x04C11DB7 反射表），初值/终值 crc ^ ~0U。
   用于 OTA 固件完整性校验，保证 Bootloader 与 App 端（ota.c）计算一致。 */
uint32_t boot_calc_crc32(uint32_t crc, const void *buf, size_t size);

#endif /* BOOT_CRC_H */
