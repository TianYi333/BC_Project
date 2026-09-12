/*
 * @file           : main_boot.c
 * @brief          : Bootloader 入口（OTA 烧录 + A/B 回滚）
 *
 * 流程：HAL_Init → MX_SPI1_Init(flash) → 读 ota_meta
 *   - PENDING : 校验 firmware_a CRC → 备份当前App→bak → 擦内部Flash→烧firmware_a
 *               → 校验内部Flash CRC → 置 DONE；失败则回滚 bak 置 FAILED
 *   - DONE    : 上次烧了但 App 未确认(起不来) → 用 bak 回滚 → 置 NONE
 *   - CONFIRMED: 升级完成 → 清标记 NONE
 *   - NONE/FAIL: 直接跳 App
 * 最后校验 App 向量表有效性并跳转。
 */
#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_flash.h"
#include "stm32h7xx_hal_flash_ex.h"
#include "spi.h"
#include "ota_common.h"
#include "boot_w25q.h"
#include "boot_crc.h"
#include "gpio.h"
#include <string.h>

static void boot_uart_puts(const char *s);   /* 前置声明：定义见下方调试串口段 */

/* 给 SPI1 提供内核时钟：boot 无 SystemClock_Config，复位后 SPI123SEL 指向未启用的
 * PLL1(pll1_q_ck)，SCK 不翻转 → W25Q 读回全 0xFF（magic 不匹配）。
 * STM32H7 的 SPI123 内核时钟不支持直连 HSI，可选源为 pll1_q/pll2_p/pll3_p/I2S_CKIN/PER_CK。
 * 这里用 HAL 配置并启动 PLL3，输入源复用复位默认(PLLCKSELR.PLLSRC=00=HSI 64MHz)，
 * 再把 SPI123 内核时钟切到 PLL3，与 App 端 PeriphCommonClock_Config 做法一致
 * （App 的 PLL3 由 HSE 驱动，此处改由 HSI 驱动，仅时钟源不同、逻辑相同）。
 *
 * PLL3 参数：DIVM3=8 → 输入 64/8=8MHz(落 VCIRANGE_2)；DIVN3=100 → VCO=800MHz(≤836MHz)；
 * DIVP3=8 → PLL3P=100MHz 即 SPI 内核时钟；SPI 波特率预分频 16 → SCK≈6.25MHz，适配 W25Q。 */
static void boot_clock_init(void)
{
    RCC_PeriphCLKInitTypeDef pc = {0};

    pc.PeriphClockSelection = RCC_PERIPHCLK_SPI123;
    pc.PLL3.PLL3M     = 8;
    pc.PLL3.PLL3N     = 100;
    pc.PLL3.PLL3P     = 8;
    pc.PLL3.PLL3Q     = 4;
    pc.PLL3.PLL3R     = 4;
    pc.PLL3.PLL3RGE   = RCC_PLL3VCIRANGE_2;
    pc.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
    pc.PLL3.PLL3FRACN = 0;
    pc.Spi123ClockSelection = RCC_SPI123CLKSOURCE_PLL3;

    if (HAL_RCCEx_PeriphCLKConfig(&pc) != HAL_OK)
    {
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] CLK FAIL\r\n");
#endif
    }
#ifdef BOOT_DEBUG_UART
    else
        boot_uart_puts("[BOOT] CLK OK\r\n");
#endif
}

/* ===================== 复位源打印 + IWDG 喂狗（诊断/防护） ===================== */
static void boot_kick_iwdg(void)
{
    /* 若 IWDG 已启动（硬件模式从复位自启），周期性写 0xAAAA 重载计数器，
       防止 boot 长耗时烧录期间被看门狗复位。软件模式下 IWDG 未启动，写此值无害。 */
    IWDG1->KR = 0xAAAAU;
}

static void boot_print_reset_cause(void)
{
#ifdef BOOT_DEBUG_UART
    uint32_t rsr = RCC->RSR;
    boot_uart_puts("[BOOT] RSR=0x");
    boot_uart_puthex(rsr, 8);
    boot_uart_puts(" ");
    if (rsr & (1UL << 26)) boot_uart_puts("IWDG1 ");
    if (rsr & (1UL << 28)) boot_uart_puts("WWDG1 ");
    if (rsr & (1UL << 24)) boot_uart_puts("SFT ");
    if (rsr & (1UL << 23)) boot_uart_puts("POR ");
    if (rsr & (1UL << 22)) boot_uart_puts("PIN ");
    if (rsr & (1UL << 25)) boot_uart_puts("BOR ");
    boot_uart_puts("\r\n");
    RCC->RSR = (1UL << 16);   /* RMVF=1 清除复位状态，便于下次判断 */
#endif
}

/* ===================== Boot 调试串口（诊断用，稳定后删除） =====================
 * 复用 App 控制台 USART2（PD5=TX, PD6=RX, AF7, 115200），纯寄存器驱动，
 * 不依赖 HAL_UART 模块。仅用于定位 OTA 卡在 PENDING 的问题；确认根因并修复后，
 * 删除本段 + main()/do_update() 里的 BOOT_DEBUG_UART 打印即可。
 */
#define BOOT_DEBUG_UART

#ifdef BOOT_DEBUG_UART
static void boot_uart_init(void)
{
    /* USART2 内核时钟选 HSI(64MHz)，与 APB 分频无关，BRR 固定为 555 */
    RCC->D2CCIP2R = (RCC->D2CCIP2R & ~(0x7UL << 0)) | (0x3UL << 0); /* USART28SEL = HSI */

    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIODEN;     /* GPIOD clock */
    RCC->APB1LENR |= RCC_APB1LENR_USART2EN;   /* USART2 clock (APB1L) */

    /* PD5 -> AF7 (USART2_TX) */
    GPIOD->MODER   = (GPIOD->MODER   & ~(3UL << 10)) | (2UL << 10);
    GPIOD->OTYPER &= ~(1UL << 5);
    GPIOD->OSPEEDR |= (3UL << 10);
    GPIOD->PUPDR   |= (1UL << 10);            /* pull-up */
    GPIOD->AFR[0]  = (GPIOD->AFR[0]  & ~(0xFUL << 20)) | (7UL << 20);

    USART2->CR1 = 0;                          /* 关 UART，同时清 FIFOEN */
    USART2->BRR = 555;                        /* 64MHz / 115200 */
    USART2->CR1 = (1UL << 3) | (1UL << 0);    /* TE | UE (8N1, 无 FIFO) */
}

static void boot_uart_putc(char c)
{
    while (!(USART2->ISR & (1UL << 7))) {}    /* 等 TXE */
    USART2->TDR = (uint8_t)c;
}
static void boot_uart_puts(const char *s)
{
    while (*s) boot_uart_putc(*s++);
}
static void boot_uart_puthex(uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9]; int i;
    for (i = 0; i < digits; i++) buf[digits - 1 - i] = hex[(v >> (4 * i)) & 0xF];
    buf[digits] = 0;
    boot_uart_puts(buf);
}
static void boot_uart_putdec(uint32_t v)
{
    char buf[12]; int i = 0;
    if (v == 0) { boot_uart_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) boot_uart_putc(buf[--i]);
}
#else
#define boot_uart_init()          ((void)0)
#define boot_uart_puts(s)         ((void)0)
#define boot_uart_puthex(v,d)     ((void)0)
#define boot_uart_putdec(v)       ((void)0)
#endif

#define APP_ADDRESS        ((uint32_t)0x08100000)
#define APP_STACK_TOP      (*((volatile uint32_t *)APP_ADDRESS))
#define APP_RESET_HANDLER  (*((volatile uint32_t *)(APP_ADDRESS + 4)))
#define FLASH_BUF_SIZE     256

/* 内部 Flash App 区（BANK2）容量。STM32H743ZGT6 总 Flash 1MB，双 bank 各 512KB，
 * BANK2 即 0x08100000 ~ 0x0817FFFF。
 * 必须与 OTA_FW_PART_SIZE（1MB，SPI Flash 分区大小）区分开：
 * 备份/回滚若按 1MB 操作内部 Flash，会越界访问不存在的地址——写会超时失败，
 * 读会取回无效数据。故凡涉及内部 Flash 的长度一律用本常量。 */
#define APP_FLASH_SIZE     (512UL * 1024UL)

/* ---------------- 最小板级初始化（boot 不依赖 App 的 gpio.c / stm32h7xx_it.c） ---------------- */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void Error_Handler(void)
{
    while (1)
    {
        __NOP();
    }
}

/* 覆写默认 HardFault：打印状态寄存器 + 故障 PC，便于一锤定音（而非静默死循环） */
void HardFault_Handler(void)
{
#ifdef BOOT_DEBUG_UART
    uint32_t msp = __get_MSP();
    uint32_t *stk = (uint32_t *)msp;
    uint32_t lr = stk[5];
    /* 无 FPU 栈帧(EXC_RETURN[4]=1)：PC 在 stk[6]；有 FPU 栈帧(EXC_RETURN[4]=0)：PC 在 msp+96 */
    uint32_t pc = (lr & 0x10U) ? stk[6] : (*((uint32_t *)(msp + 96U)));
    boot_uart_puts("[BOOT] HARDFAULT\r\n");
    boot_uart_puts("[BOOT] HFSR=0x"); boot_uart_puthex(SCB->HFSR, 8);
    boot_uart_puts(" CFSR=0x"); boot_uart_puthex(SCB->CFSR, 8);
    boot_uart_puts("\r\n[BOOT] MMFAR=0x"); boot_uart_puthex(SCB->MMFAR, 8);
    boot_uart_puts(" BFAR=0x"); boot_uart_puthex(SCB->BFAR, 8);
    boot_uart_puts("\r\n[BOOT] PC=0x"); boot_uart_puthex(pc, 8);
    boot_uart_puts(" LR=0x"); boot_uart_puthex(lr, 8);
    boot_uart_puts(" MSP=0x"); boot_uart_puthex(msp, 8);
    boot_uart_puts("\r\n");
#endif
    while (1) __NOP();
}

/* 仅把 SPI1 软件片选(SPI1_NSS)配成 GPIO 输出、空闲高电平；
   SPI1 的 SCK/MISO/MOSI 由 stm32h7xx_hal_msp.c 的 HAL_SPI_MspInit 配置。
   不开其它 GPIO / EXTI，保持 boot 最小。 */
static void boot_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin  = SPI1_NSS_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_SET);
    HAL_GPIO_Init(SPI1_NSS_GPIO_Port, &gpio);
}

/* ---------------- 内部 Flash 操作 ---------------- */

static void JumpToApp(void)
{
    __disable_irq();
    __set_MSP(APP_STACK_TOP);
    SCB->VTOR = APP_ADDRESS;
    ((void (*)(void))APP_RESET_HANDLER)();
}

static int app_valid(void)
{
    return (APP_STACK_TOP >= 0x24000000UL && APP_STACK_TOP < 0x24100000UL) &&
           (APP_RESET_HANDLER >= APP_ADDRESS && APP_RESET_HANDLER < 0x08200000UL);
}

/* ===================== 寄存器级 BANK2 内部 Flash 操作 =====================
 * 绕过 HAL 的 FLASH_WaitForLastOperation（轮询 QW，超时 50s 静默干等）。
 * 改用 HAL_GetTick() 计时 + 每 1s 心跳打印 + 喂狗的有界等待，操作前显式清除
 * SR2 错误标志；一旦 WRPERR/PGSERR/OPERR 置位立即打印根因。
 * FLASH->CR2/SR2/CCR2/KEYR2 与 BANK1 位布局镜像，所用宏已在 CMSIS 头核准。 */
#define BOOT_FLASH_KEY1  0x45670123U
#define BOOT_FLASH_KEY2  0xCDEF89ABU

static void flash_unlock(void)
{
    if (FLASH->CR2 & FLASH_CR_LOCK) { FLASH->KEYR2 = BOOT_FLASH_KEY1; FLASH->KEYR2 = BOOT_FLASH_KEY2; }
}

static void flash_lock(void)
{
    FLASH->CR2 |= FLASH_CR_LOCK;
}

/* 等待 BANK2 操作完成（轮询 EOP 完成锁存；出错立即返回并打印）。绝不静默卡 50s。 */
static int flash_wait_bank2(const char *tag, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint32_t hb = t0;
    for (;;)
    {
        boot_kick_iwdg();
        uint32_t sr = FLASH->SR2;
        if (sr & (FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_OPERR | FLASH_SR_STRBERR | FLASH_SR_INCERR))
        {
            boot_uart_puts("[BOOT] FLASH ERR sr2=0x");
            boot_uart_puthex(sr, 8);
            if (sr & FLASH_SR_WRPERR) boot_uart_puts(" WRPERR");
            if (sr & FLASH_SR_PGSERR) boot_uart_puts(" PGSERR");
            if (sr & FLASH_SR_OPERR)  boot_uart_puts(" OPERR");
            boot_uart_puts("\r\n");
            FLASH->CCR2 = (FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_OPERR |
                           FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_EOP);
            return -1;
        }
        if (sr & FLASH_SR_EOP)
        {
            FLASH->CCR2 = FLASH_CCR_CLR_EOP;
            return 0;
        }
        uint32_t now = HAL_GetTick();
        if ((int32_t)(now - hb) >= 1000) { hb = now; boot_uart_puts(tag); }
        if ((int32_t)(now - t0) >= (int32_t)timeout_ms) { boot_uart_puts("[BOOT] FLASH WAIT TIMEOUT\r\n"); return -1; }
    }
}

/* 整片擦除 BANK2（即 App 内部 Flash 区） */
static int erase_app_internal(void)
{
    flash_unlock();
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] flash unlock ok\r\n");
#endif
    /* boot 上电复位后 D-Cache/I-Cache 默认关闭且全程不开启，Flash 读回直接走 AXIM，无缓存一致性问题；
     * 实测在此处调用 SCB_DisableDCache()/SCB_DisableICache() 会触发 HardFault（曾引发 backup 后卡死），故移除。 */
    /* 操作前清所有错误标志（HAL 不保证） */
    FLASH->CCR2 = (FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_OPERR |
                   FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_EOP);
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] mass-erase BANK2...\r\n");
#endif
    FLASH->CR2 &= ~FLASH_CR_PSIZE;                  /* H7 对 mass-erase/FLASHWORD 忽略 PSIZE */
    FLASH->CR2 |= (FLASH_CR_BER | FLASH_CR_START); /* 整片擦除 BANK2 */
    if (flash_wait_bank2("[BOOT] erase wait... ", 15000U) != 0) { flash_lock(); return -1; }
    FLASH->CR2 &= ~FLASH_CR_BER;
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] erase done\r\n");
#endif
    return 0;
}

/* 将 SPI 分区(spi_off) 的 len 字节烧入内部 Flash（先擦后写）。
   注意：STM32H7 内部 Flash 以 256-bit(32 字节) flash word 为单位编程，
   HAL_FLASH_Program 仅支持 FLASH_TYPEPROGRAM_FLASHWORD，且 FlashAddress
   必须 32 字节对齐；DataAddress 为 RAM 中 32 字节对齐缓冲区地址。 */
static int burn_spi_to_internal(uint32_t spi_off, uint32_t len)
{
    uint8_t buf[32] __attribute__((aligned(32)));
    uint32_t off = 0;
    uint32_t remaining = len;
    uint32_t addr = APP_ADDRESS;

    /* 容量保护：BANK2 只有 512KB，超长一律拒绝，绝不越界访问不存在的 Flash 地址 */
    if (len == 0 || len > APP_FLASH_SIZE)
    {
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] burn len OUT OF RANGE\r\n");
#endif
        return -1;
    }

    if (erase_app_internal() != 0)
    {
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] erase FAIL\r\n");
#endif
        return -1;
    }
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] erased, burning @0x");
    boot_uart_puthex(addr, 8);
    boot_uart_puts("\r\n");
#endif

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > 32U) ? 32U : remaining;

        w25q_read(spi_off + off, buf, chunk);
        if (chunk < 32U)
        {
            memset(buf + chunk, 0xFF, 32U - chunk);   /* 尾块按擦除值补齐 */
        }

        /* 寄存器级 FLASHWORD(256bit/32字节) 编程 BANK2：清错误→置 PG→写满 8×32bit→等 EOP→清 PG */
        FLASH->CCR2 = (FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_OPERR |
                       FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_EOP);
        FLASH->CR2 |= FLASH_CR_PG;
        {
            volatile uint32_t *dst = (volatile uint32_t *)addr;
            uint32_t *src = (uint32_t *)buf;          /* buf 已 32 字节对齐 */
            dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3];
            dst[4]=src[4]; dst[5]=src[5]; dst[6]=src[6]; dst[7]=src[7];
        }
        if (flash_wait_bank2("[BOOT] prog wait... ", 3000U) != 0)
        {
            FLASH->CR2 &= ~FLASH_CR_PG;
            flash_lock();
#ifdef BOOT_DEBUG_UART
            boot_uart_puts("[BOOT] burn FAIL\r\n");
#endif
            return -1;
        }
        FLASH->CR2 &= ~FLASH_CR_PG;

        addr += 32U;
        off  += chunk;
        remaining -= chunk;
        boot_kick_iwdg();   /* 长烧录期间喂狗，防看门狗复位 */
#ifdef BOOT_DEBUG_UART
        if ((off & 0xFFFFUL) == 0)   /* 每 64KB 打印一次进度（off 为已烧字节数） */
        {
            boot_uart_puts("[BOOT] burned ");
            boot_uart_putdec(off / 1024);
            boot_uart_puts("KB\r\n");
        }
#endif
    }

    flash_lock();
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] burn ok\r\n");
#endif
    return 0;
}

/* 备份当前内部 Flash App 到 firmware_bak（SPI） */
static int backup_app_to_bak(void)
{
    uint8_t buf[FLASH_BUF_SIZE];
    uint32_t addr = APP_ADDRESS;
    uint32_t remaining = APP_FLASH_SIZE;
    uint32_t off = 0;

    for (uint32_t b = 0; b < APP_FLASH_SIZE; b += (64UL * 1024UL))
    {
        w25q_erase_block64k(OTA_FW_BAK_OFFSET + b);
    }

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > FLASH_BUF_SIZE) ? FLASH_BUF_SIZE : remaining;

        memcpy(buf, (const void *)addr, chunk);
        w25q_write(OTA_FW_BAK_OFFSET + off, buf, chunk);

        addr += chunk;
        off += chunk;
        remaining -= chunk;
        boot_kick_iwdg();   /* 长备份期间喂狗 */
    }
    return 0;
}

static uint32_t calc_internal_crc(uint32_t len)
{
    uint32_t crc = 0;
    uint8_t buf[FLASH_BUF_SIZE];
    uint32_t addr = APP_ADDRESS;
    uint32_t remaining = len;

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > FLASH_BUF_SIZE) ? FLASH_BUF_SIZE : remaining;

        memcpy(buf, (const void *)addr, chunk);
        crc = boot_calc_crc32(crc, buf, chunk);
        addr += chunk;
        remaining -= chunk;
    }
    return crc;
}

static uint32_t calc_spi_crc(uint32_t spi_off, uint32_t len)
{
    uint32_t crc = 0;
    uint8_t buf[FLASH_BUF_SIZE];
    uint32_t off = 0;
    uint32_t remaining = len;

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > FLASH_BUF_SIZE) ? FLASH_BUF_SIZE : remaining;

        w25q_read(spi_off + off, buf, chunk);
        crc = boot_calc_crc32(crc, buf, chunk);
        off += chunk;
        remaining -= chunk;
    }
    return crc;
}

static void write_meta(const ota_meta_t *meta)
{
    w25q_erase_sector4k(OTA_META_OFFSET);
    w25q_write(OTA_META_OFFSET, (const uint8_t *)meta, sizeof(ota_meta_t));
}

static void do_update(ota_meta_t *meta)
{
    /* 1. 校验 firmware_a 完整性 */
    uint32_t crc = calc_spi_crc(OTA_FW_A_OFFSET, meta->new_len);
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] do_update: crc_spi=");
    boot_uart_puthex(crc, 8);
    boot_uart_puts(" exp=");
    boot_uart_puthex(meta->new_crc32, 8);
    boot_uart_puts(" len=");
    boot_uart_putdec(meta->new_len);
    boot_uart_puts("\r\n[BOOT] ** DO NOT POWER OFF, updating (~1min) **\r\n");
#endif
    if (crc != meta->new_crc32)
    {
        meta->state = OTA_STATE_FAILED;
        write_meta(meta);
        return;
    }

    /* 2. 备份当前 App 到 firmware_bak */
    backup_app_to_bak();
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] backup done\r\n");
#endif

    /* 3. 擦内部 Flash 并烧入 firmware_a */
    if (burn_spi_to_internal(OTA_FW_A_OFFSET, meta->new_len) != 0)
    {
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] burn stage FAIL -> rollback\r\n");
#endif
        burn_spi_to_internal(OTA_FW_BAK_OFFSET, APP_FLASH_SIZE); /* 回滚 */
        meta->state = OTA_STATE_FAILED;
        write_meta(meta);
        return;
    }

    /* 4. 校验内部 Flash CRC */
    if (calc_internal_crc(meta->new_len) != meta->new_crc32)
    {
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] verify MISMATCH -> rollback\r\n");
#endif
        burn_spi_to_internal(OTA_FW_BAK_OFFSET, APP_FLASH_SIZE);
        meta->state = OTA_STATE_FAILED;
    }
    else
    {
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] verify ok\r\n");
#endif
        meta->state = OTA_STATE_DONE;
    }
    write_meta(meta);
#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] meta=");
    boot_uart_putdec(meta->state);
    boot_uart_puts(" written\r\n");
#endif
}

int main(void)
{
    HAL_Init();
    boot_clock_init();          /* 配置并启动 PLL3(HSI 输入)，把 SPI123 内核时钟切到 PLL3，否则 SCK 不翻转、W25Q 读回 0xFF */
#ifdef BOOT_DEBUG_UART
    boot_uart_init();
    boot_uart_puts("[BOOT] START\r\n");
    boot_print_reset_cause();
#endif
    boot_gpio_init();
    MX_SPI1_Init();

    /* W25Q64 上电有 tPUW（最长约 10ms）稳定时间；断电重启时 boot 启动极快，
       若立即可读会拿到 0xFF/乱码，导致 magic 不匹配而静默跳过 OTA。
       先延时等 W25Q 上电稳定，再带重试读取 meta，避免一次失败就跳过。 */
    HAL_Delay(30);

    ota_meta_t meta;
    uint32_t meta_retry = 0;
    do {
        w25q_read(OTA_META_OFFSET, (uint8_t *)&meta, sizeof(meta));
#ifdef BOOT_DEBUG_UART
        boot_uart_puts("[BOOT] try=");
        boot_uart_putdec(meta_retry);
        boot_uart_puts(" magic=");
        boot_uart_puthex(meta.magic, 8);
        boot_uart_puts(" state=");
        boot_uart_putdec(meta.state);
        boot_uart_puts("\r\n");
#endif
        if (meta.magic == OTA_META_MAGIC)
            break;
        HAL_Delay(20);
    } while (++meta_retry < 5);

#ifdef BOOT_DEBUG_UART
    boot_uart_puts("[BOOT] final magic=");
    boot_uart_puthex(meta.magic, 8);
    boot_uart_puts(" state=");
    boot_uart_putdec(meta.state);
    boot_uart_puts(" new_len=");
    boot_uart_putdec(meta.new_len);
    boot_uart_puts(" new_crc=");
    boot_uart_puthex(meta.new_crc32, 8);
    boot_uart_puts("\r\n");
#endif

    if (meta.magic == OTA_META_MAGIC)
    {
        switch (meta.state)
        {
        case OTA_STATE_PENDING:
            do_update(&meta);
            break;
        case OTA_STATE_DONE:
            /* 上次烧录成功但 App 未确认（起不来）→ 回滚到备份 */
            burn_spi_to_internal(OTA_FW_BAK_OFFSET, APP_FLASH_SIZE);
            meta.state = OTA_STATE_NONE;
            write_meta(&meta);
            break;
        case OTA_STATE_CONFIRMED:
            meta.state = OTA_STATE_NONE;
            write_meta(&meta);
            break;
        case OTA_STATE_NONE:
        case OTA_STATE_FAILED:
        default:
            break;
        }
    }

    if (app_valid())
    {
        JumpToApp();
    }

    while (1)
    {
        __NOP();
    }
}
