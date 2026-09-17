/*
 * @file           : system_stm32f7xx.c (Bootloader 专用)
 * @brief          : 最小系统初始化。Bootloader 驻留 0x08000000，复位后 VTOR 默认
 *                   即指向 0x08000000，无需重定位（App 才通过 VECT_TAB_OFFSET
 *                   把 VTOR 设到 0x080A0000）。仅使能 FPU，避免引入 HAL。
 */
#include "stm32f767xx.h"

/* 复位后系统时钟为 HSI 16MHz；Bootloader 不需要 PLL，Flash 编程按 16MHz 即可。 */
uint32_t SystemCoreClock = 16000000;

void SystemInit(void)
{
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    SCB->CPACR |= ((3UL << (10U * 2U)) | (3UL << (11U * 2U)));  /* CP10/CP11 全访问 */
#endif

    /* Bootloader 不重定位向量表：VTOR 保持复位默认值（指向 0x08000000）。 */
}

void SystemCoreClockUpdate(void)
{
    /* Bootloader 不配置系统时钟，保持 HSI 16MHz。 */
    SystemCoreClock = 16000000;
}
