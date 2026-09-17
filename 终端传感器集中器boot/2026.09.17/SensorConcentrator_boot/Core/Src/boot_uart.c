/*
 * @file           : boot_uart.c
 * @brief          : Bootloader 调试串口实现（详见 boot_uart.h）
 *
 * 时钟假设：Bootloader 不配置 PLL，复位后 SYSCLK = HSI 16MHz，且 PPRE1/PPRE2
 *   默认不分频，故 PCLK1 = 16MHz。若哪天在 system_stm32f7xx.c 里开了 PLL，
 *   必须同步修改 BOOT_PCLK1_HZ，否则波特率会整体偏移。
 */
#include "boot_uart.h"
#include "stm32f767xx.h"
#include <stdarg.h>

#ifndef BOOT_PCLK1_HZ
#define BOOT_PCLK1_HZ   16000000UL     /* HSI 16MHz，PCLK1 不分频 */
#endif
#ifndef BOOT_BAUDRATE
#define BOOT_BAUDRATE   115200UL
#endif

/* 与 App 调试口同款硬件：UART7 / TX=PF7(AF8) / DE=PA5 */
#define BOOT_UART       UART7
#define BOOT_TX_PORT    GPIOF
#define BOOT_TX_PIN     7U
#define BOOT_DE_PORT    GPIOA
#define BOOT_DE_PIN     5U
#define BOOT_GPIO_AF    8U             /* GPIO_AF8_UART7 */

static volatile uint8_t s_inited = 0;

/* ------------------------------ 硬件层 ------------------------------ */
static void de_high(void) { BOOT_DE_PORT->BSRR = (1UL << BOOT_DE_PIN); }
static void de_low(void)  { BOOT_DE_PORT->BSRR = (1UL << (BOOT_DE_PIN + 16U)); }

static void uart_putc_raw(char c)
{
    while ((BOOT_UART->ISR & USART_ISR_TXE) == 0U) { }
    BOOT_UART->TDR = (uint32_t)(uint8_t)c;
}

/* 等最后一字节真正移出后再拉低 DE，避免 RS485 帧尾被截断 */
static void uart_wait_tc(void)
{
    while ((BOOT_UART->ISR & USART_ISR_TC) == 0U) { }
}

void boot_uart_init(void)
{
    if (s_inited) return;
    s_inited = 1;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOFEN;
    RCC->APB1ENR |= RCC_APB1ENR_UART7EN;
    (void)RCC->AHB1ENR;   /* 读回，确保使能生效（时钟门控写后同步） */
    (void)RCC->APB1ENR;

    /* DE(PA5)：推挽输出，默认低 = 接收态 */
    BOOT_DE_PORT->BSRR  = (1UL << (BOOT_DE_PIN + 16U));
    BOOT_DE_PORT->MODER = (BOOT_DE_PORT->MODER & ~(3UL << (BOOT_DE_PIN * 2U)))
                        | (1UL << (BOOT_DE_PIN * 2U));               /* 01 = 输出 */
    BOOT_DE_PORT->OTYPER &= ~(1UL << BOOT_DE_PIN);                    /* 推挽 */
    BOOT_DE_PORT->OSPEEDR |= (3UL << (BOOT_DE_PIN * 2U));            /* 高速 */
    BOOT_DE_PORT->PUPDR &= ~(3UL << (BOOT_DE_PIN * 2U));             /* 无上下拉 */

    /* TX(PF7)：复用功能 AF8 */
    BOOT_TX_PORT->MODER = (BOOT_TX_PORT->MODER & ~(3UL << (BOOT_TX_PIN * 2U)))
                        | (2UL << (BOOT_TX_PIN * 2U));               /* 10 = AF */
    BOOT_TX_PORT->OTYPER &= ~(1UL << BOOT_TX_PIN);
    BOOT_TX_PORT->OSPEEDR |= (3UL << (BOOT_TX_PIN * 2U));
    BOOT_TX_PORT->PUPDR &= ~(3UL << (BOOT_TX_PIN * 2U));
    BOOT_TX_PORT->AFR[0] = (BOOT_TX_PORT->AFR[0] & ~(0xFUL << (BOOT_TX_PIN * 4U)))
                         | (BOOT_GPIO_AF << (BOOT_TX_PIN * 4U));

    /* UART7：8N1，OVER8=0，仅发送 */
    BOOT_UART->CR1 = 0U;                                             /* M=0,PCE=0,UE=0 */
    BOOT_UART->CR2 = 0U;                                             /* 1 stop bit */
    BOOT_UART->CR3 = 0U;
    {
        uint32_t div = (BOOT_PCLK1_HZ + (BOOT_BAUDRATE / 2U)) / BOOT_BAUDRATE;
        BOOT_UART->BRR = div & 0xFFFFU;
    }
    BOOT_UART->CR1 |= USART_CR1_TE | USART_CR1_UE;
}

/* ------------------------------ 格式化 ------------------------------ */
static void emit_str(const char *s)
{
    while (*s) { uart_putc_raw(*s++); }
}

static void emit_uint(unsigned long v, unsigned base, int width, int zero, int upper)
{
    char buf[24];
    int i = 0;

    do {
        unsigned long d = v % (unsigned long)base;
        buf[i++] = (char)((d < 10UL) ? ('0' + d) : ((upper ? 'A' : 'a') + (d - 10UL)));
        v /= (unsigned long)base;
    } while ((v != 0UL) && (i < (int)sizeof(buf)));

    while ((i < width) && (i < (int)sizeof(buf))) {
        buf[i++] = (char)(zero ? '0' : ' ');
    }
    while (i > 0) { uart_putc_raw(buf[--i]); }
}

void boot_printf(const char *fmt, ...)
{
    va_list ap;
    const char *p;

    if (!s_inited) return;

    va_start(ap, fmt);

    /* 整行一次发完：先拉高 DE，末尾等 TC 再拉低（RS485 半双工） */
    BOOT_UART->ICR = USART_ICR_TCCF;       /* 清旧 TC，保证末尾等待有效 */
    de_high();

    for (p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            if (*p == '\n') uart_putc_raw('\r');
            uart_putc_raw(*p);
            continue;
        }

        int zero = 0, width = 0;
        p++;
        if (*p == '0') { zero = 1; p++; }
        while ((*p >= '0') && (*p <= '9')) {
            width = (width * 10) + (*p - '0');
            p++;
        }
        while (*p == 'l' || *p == 'L') { p++; }   /* 忽略长度修饰符 */

        switch (*p) {
        case 's':
            emit_str(va_arg(ap, const char *));
            break;
        case 'c':
            uart_putc_raw((char)va_arg(ap, int));
            break;
        case 'd':
        case 'i':
        {
            long v = va_arg(ap, long);
            unsigned long u;
            if (v < 0L) { uart_putc_raw('-'); u = (unsigned long)(-(v + 1L)) + 1UL; }
            else        { u = (unsigned long)v; }
            emit_uint(u, 10U, width, zero, 0);
            break;
        }
        case 'u':
            emit_uint(va_arg(ap, unsigned long), 10U, width, zero, 0);
            break;
        case 'x':
            emit_uint(va_arg(ap, unsigned long), 16U, width, zero, 0);
            break;
        case 'X':
            emit_uint(va_arg(ap, unsigned long), 16U, width, zero, 1);
            break;
        case 'p':
            emit_str("0x");
            emit_uint(va_arg(ap, unsigned long), 16U, 8, 1, 1);
            break;
        case '%':
            uart_putc_raw('%');
            break;
        default:
            uart_putc_raw('%');
            uart_putc_raw(*p);
            break;
        }
    }

    uart_wait_tc();
    de_low();

    va_end(ap);
}

/* ------------------------- FLASH->SR 错误位解码 ------------------------- */
void boot_dump_flash_sr(const char *tag, uint32_t sr)
{
    boot_printf("[BOOT] %s: SR=0x%08X ->", tag, (unsigned long)sr);
    if ((sr & FLASH_SR_OPERR)  != 0U) boot_printf(" OPERR");
    if ((sr & FLASH_SR_WRPERR) != 0U) boot_printf(" WRPERR");
    if ((sr & FLASH_SR_PGAERR) != 0U) boot_printf(" PGAERR");
    if ((sr & FLASH_SR_PGPERR) != 0U) boot_printf(" PGPERR");
    if ((sr & FLASH_SR_ERSERR) != 0U) boot_printf(" ERSERR");
    if ((sr & FLASH_SR_BSY)    != 0U) boot_printf(" BSY");
    if ((sr & (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR |
               FLASH_SR_PGPERR | FLASH_SR_ERSERR)) == 0U) boot_printf(" no-error");
    boot_printf("\n");
}
