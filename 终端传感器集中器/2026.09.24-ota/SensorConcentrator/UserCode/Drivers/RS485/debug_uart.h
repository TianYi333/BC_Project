/**
  ******************************************************************************
  * @file    debug_uart.h
  * @brief   临时调试串口：占用一路 RS485 作为 printf / LOG_* 的输出通道
  *
  * 背景：工程里 syscalls.c 只有 `extern int __io_putchar(int ch) __attribute__((weak));`
  *       的弱声明，全项目没有强实现，因此 printf() 与所有 LOG_* 宏实际上没有任何
  *       输出通道。本模块提供 __io_putchar() 的强定义，把输出重定向到
  *       SH_DEBUG_UART_ID 指定的那一路 RS485。
  *
 * 用法：
 *   1) 在 sensor_hub.h 里用总开关宏 DEBUG_UART_ENABLE 控制：
 *        =1（默认） 开发调试，CH1（UART7）被占用为调试口；
 *        =0         正式运行，不占用任何路，CH1 恢复传感器采集，调试打印全部关闭；
 *   2) 需要换调试口路号时再改 SH_DEBUG_UART_ID（默认 1，即 CH1）；
 *   3) 该路不再参与传感器采集（SensorHub_Init 会跳过它）；
 *   4) 直接 printf() / LOG_Debug() / LOG_logic() 即可从该路输出。
  ******************************************************************************
  */
#ifndef __DEBUG_UART_H
#define __DEBUG_UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "usart.h"

/* 占用 RS485 的哪一路由 sensor_hub.h 的 SH_DEBUG_UART_ID 决定 */

/* 行缓冲大小：满或遇到 '\n' 时一次性发出 */
#define DEBUG_TX_BUF_SIZE       256

/* 初始化调试口（在任务上下文中调用；内部防重入） */
void DebugUart_Init(void);

/* 立即把行缓冲发出去（正常由 '\n' 自动触发，一般无需手动调用） */
void DebugUart_Flush(void);

/* 以 hex 形式打印一段数据，用于查看 485 收到的原始帧 */
void DebugUart_DumpHex(const char *tag, const uint8_t *data, uint16_t len);

/* 异常入口用的安全打印（不依赖 printf / 堆 / 互斥量） */
void DebugUart_Panic(const char *msg);

/* 异常入口用的 32 位十六进制打印（不依赖 printf / 堆 / 互斥量），用于 dump SCB 寄存器 */
void DebugUart_PanicHex(const char *label, uint32_t val);

/* syscalls.c 中 _write() 调用的弱符号，此处提供强定义以接管 printf */
int __io_putchar(int ch);

#ifdef __cplusplus
}
#endif

#endif /* __DEBUG_UART_H */
