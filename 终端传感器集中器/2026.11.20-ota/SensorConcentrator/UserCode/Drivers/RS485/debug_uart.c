/**
  ******************************************************************************
  * @file    debug_uart.c
  * @brief   临时调试串口实现（详见 debug_uart.h）
  *
  * 设计要点：
  *   - RS485 是半双工，收发由 DE 引脚切换，不能按字符逐个切方向（会撕裂且极慢），
  *     因此采用「行缓冲」：字符先攒入 s_tx_buf，遇到 '\n' 或缓冲满时才切 DE 发送一次。
  *   - printf 可能被多个任务调用，发送过程用互斥量保护，避免两帧数据交错。
  *   - 未初始化（s_huart == NULL）时直接丢弃，保证启动早期调用 printf 不会出错。
  ******************************************************************************
  */
#include "debug_uart.h"
#include "sensor_hub.h"

#if SH_DEBUG_UART_ID

#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/reent.h>

static UART_HandleTypeDef *s_huart   = NULL;
static GPIO_TypeDef       *s_de_port = NULL;
static uint16_t            s_de_pin  = 0;

static uint8_t  s_tx_buf[DEBUG_TX_BUF_SIZE];
static uint16_t s_tx_len = 0;
static SemaphoreHandle_t s_tx_mutex = NULL;
static uint8_t  s_inited = 0;

/* ============================ 初始化 ============================ */
void DebugUart_Init(void)
{
    if (s_inited) return;          /* 防重入 */
    s_inited = 1;

    /* 从采集层取该路 UART 与方向引脚硬件信息 */
    SensorHub_GetPortHw((uint8_t)SH_DEBUG_UART_ID, &s_huart, &s_de_port, &s_de_pin);
    if (s_huart == NULL) return;

    /* 调试口是 TX-only 控制台：DE 常置发送态，避免空闲时总线无驱动产生噪声乱码 */
    HAL_GPIO_WritePin(s_de_port, s_de_pin, GPIO_PIN_SET);

    s_tx_mutex = xSemaphoreCreateMutex();

    /* 上电立即打印，用来确认调试口链路是否真的打通 */
    printf("\r\n[DBG] RS485 #%u debug port ready, %lu 8N1\r\n",
           (unsigned)SH_DEBUG_UART_ID,
           (unsigned long)s_huart->Init.BaudRate);
    printf("[DBG] heap total: %u bytes\r\n", (unsigned)configTOTAL_HEAP_SIZE);
}

/* ============================ 底层发送 ============================ */
static void debug_uart_send(const uint8_t *data, uint16_t len)
{
    if (s_huart == NULL || len == 0) return;

    /* DE 已在初始化时常置发送态（调试口 TX-only），直接阻塞发送即可 */
    HAL_UART_Transmit(s_huart, (uint8_t *)data, len, 100);
}

/* ============================ 刷缓冲 ============================ */
void DebugUart_Flush(void)
{
    if (s_tx_len == 0) return;

    if (s_tx_mutex != NULL) xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    uint16_t n = s_tx_len;
    s_tx_len = 0;
    debug_uart_send(s_tx_buf, n);

    if (s_tx_mutex != NULL) xSemaphoreGive(s_tx_mutex);
}

/* ============================ 单字符入行缓冲 ============================ */
static void debug_putc(char ch)
{
    if (s_huart == NULL) return;                 /* 尚未初始化，直接丢弃 */

    if (s_tx_len < DEBUG_TX_BUF_SIZE)
    {
        s_tx_buf[s_tx_len++] = (uint8_t)ch;
    }

    /* 换行或缓冲满 → 立即发出这一帧 */
    if (ch == '\n' || s_tx_len >= DEBUG_TX_BUF_SIZE)
    {
        DebugUart_Flush();
    }
}

/* ============================ printf 输出出口 ============================ */

/* 路径一：syscalls.c 的 _write() 会调用它（weak 声明，此处强定义）。
 * 加 used 属性，避免它因当前无强引用而被 --gc-sections 回收。 */
__attribute__((used)) int __io_putchar(int ch)
{
    debug_putc((char)ch);
    return ch;
}

/* 路径二（关键）：本工程链接 --specs=nano.specs，printf() 的真正出口是 newlib 的
 * _write_r()，而不是 syscalls.c 里那个无人引用的 weak _write()。
 * 若不在此提供 _write_r() 强定义，_write() 与 __io_putchar() 都会被
 * -Wl,--gc-sections 整段丢弃，表现就是串口完全无输出。
 */
_ssize_t _write_r(struct _reent *r, int fd, const void *ptr, size_t len)
{
    (void)r;
    if (fd != 1 && fd != 2)                      /* 只处理 stdout / stderr */
    {
        return (_ssize_t)len;
    }

    const char *p = (const char *)ptr;
    for (size_t i = 0; i < len; i++)
    {
        debug_putc(p[i]);
    }
    return (_ssize_t)len;
}

/* ============================ hex 打印 ============================ */
void DebugUart_DumpHex(const char *tag, const uint8_t *data, uint16_t len)
{
    if (s_huart == NULL) return;
    printf("[%s] len=%u: ", tag, (unsigned)len);
    for (uint16_t i = 0; i < len; i++)
    {
        printf("%02X ", data[i]);
    }
    printf("\r\n");
}

/* ============================ 异常诊断打印 ============================
 * HardFault/BusFault/MemManage 等异常入口调用。不依赖 printf / 堆 / 互斥量，
 * 直接轮询发送字符串，帮助判断异常类型与发生位置。
 */
void DebugUart_Panic(const char *msg)
{
    if (s_huart == NULL) return;

    HAL_GPIO_WritePin(s_de_port, s_de_pin, GPIO_PIN_SET);
    HAL_UART_Transmit(s_huart, (uint8_t *)msg, (uint16_t)strlen(msg), 100);
}

/* 异常诊断专用：把 "label=0x12345678\r\n" 直接轮询发出（不依赖 printf / 堆 / 互斥量）。
 * 用于 HardFault/BusFault 等异常入口打印 SCB 寄存器，精确定位崩溃点。 */
void DebugUart_PanicHex(const char *label, uint32_t val)
{
    if (s_huart == NULL) return;

    char buf[32];
    int i = 0;

    const char *p = label;
    while (*p && i < 12) buf[i++] = *p++;
    buf[i++] = '='; buf[i++] = '0'; buf[i++] = 'x';
    for (int n = 7; n >= 0; n--)
    {
        uint8_t d = (uint8_t)((val >> (n * 4)) & 0xFu);
        buf[i++] = (d < 10u) ? (char)('0' + d) : (char)('A' + d - 10u);
    }
    buf[i++] = '\r'; buf[i++] = '\n';

    HAL_GPIO_WritePin(s_de_port, s_de_pin, GPIO_PIN_SET);
    HAL_UART_Transmit(s_huart, (uint8_t *)buf, (uint16_t)i, 100);
}

#else /* SH_DEBUG_UART_ID == 0 ：调试口关闭，所有调试函数编译为空实现（不占用任何 RS485 路） */

/* 关闭调试时 CH1 已恢复为传感器采集，没有控制台可输出，以下全部为 no-op。
 * 本分支刻意不提供 _write_r / __io_putchar，使 printf 在关闭调试时无任何输出通道
 * （被 --gc-sections 丢弃），既省 Flash 又不会往传感器总线塞调试帧。
 * 调用点（main.c / sensor_hub.c / stm32f7xx_it.c / fal 驱动）仍能正常链接。 */
#include <stdint.h>

void DebugUart_Init(void) {}
void DebugUart_Flush(void) {}
void DebugUart_DumpHex(const char *tag, const uint8_t *data, uint16_t len)
{
    (void)tag; (void)data; (void)len;
}
void DebugUart_Panic(const char *msg)              { (void)msg; }
void DebugUart_PanicHex(const char *label, uint32_t val) { (void)label; (void)val; }

#endif /* SH_DEBUG_UART_ID */
