#ifndef BOOT_CRC_H
#define BOOT_CRC_H

#include <stdint.h>
#include <stddef.h>

/* 标准 CRC-32（与 FlashDB fdb_calc_crc32、上位机 zlib.crc32 逐位一致）。
 * 初值 0xFFFFFFFF，输入/输出按位反转，末异或 0xFFFFFFFF。 */
uint32_t boot_calc_crc32(uint32_t crc, const void *buf, size_t size);

#endif /* BOOT_CRC_H */
