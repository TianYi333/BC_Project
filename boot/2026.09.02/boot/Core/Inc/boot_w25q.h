#ifndef BOOT_W25Q_H
#define BOOT_W25Q_H

#include <stdint.h>
#include <stddef.h>

/* W25Q64 极简 SPI 裸驱动（供 Bootloader 使用）。
   SPI1 由 MX_SPI1_Init() 初始化，NSS 引脚用 SPI1_NSS（与 sfud_port.c 共用硬件）。
   不依赖 Fal/SFUD/RTOS，避免引入 App 端日志与 FreeRTOS 依赖。 */

/* 读取 len 字节到 buf（addr 为 W25Q64 内部地址） */
int w25q_read(uint32_t addr, uint8_t *buf, size_t len);

/* 写入 len 字节（自动按 256B 页编程、跨页拆分）。
   注意：写之前调用方必须先擦除目标区域！ */
int w25q_write(uint32_t addr, const uint8_t *buf, size_t len);

/* 擦除 64KB 块 / 4KB 扇区（addr 须对齐到块/扇区边界） */
int w25q_erase_block64k(uint32_t addr);
int w25q_erase_sector4k(uint32_t addr);

#endif /* BOOT_W25Q_H */
