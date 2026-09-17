/*
 * @file           : boot_uart.h
 * @brief          : Bootloader 调试串口（寄存器级，无 HAL / 无 newlib printf）
 *
 * 设计要点：
 *   - 打印口复用 App 的调试口（sensor_hub.h 的 SH_DEBUG_UART_ID=1 -> CH1）：
 *       UART7  TX = PF7 (AF8)   方向脚 DE = PA5（RS485 半双工）
 *     App 用的是 115200 8N1，这里保持一致，串口助手无需改配置。
 *   - 不接 newlib 的 printf：本工程链接 --specs=nano.specs 且开 -ffunction-sections
 *     + -Wl,--gc-sections，newlib 的出口是 _write_r() 而非 syscalls.c 里的
 *     __io_putchar()，容易整段被 gc 掉导致"零输出"。这里自写极简格式化，
 *     不依赖堆、不依赖互斥量、不依赖中断，Flash 擦写期间（关中断）也能打。
 *   - Boot 代码跑在 Bank1，擦写目标全在 Bank2，双 Bank RWW，故打印期间
 *     从 Bank1 取指安全；Flash 原语本身仍走 .RamFunc。
 */
#ifndef BOOT_UART_H
#define BOOT_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 UART7 + DE 引脚。可重复调用（内部防重入） */
void boot_uart_init(void);

/* 极简格式化输出，支持：%s %c %d %u %x %X %p %% （可带 0N / N 宽度，如 %08X、%4d）
 * 不支持浮点、不支持 %f。 */
void boot_printf(const char *fmt, ...);

/* 把 FLASH->SR 的错误位解码成文字打印，用于定位擦/写失败原因 */
void boot_dump_flash_sr(const char *tag, uint32_t sr);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_UART_H */
