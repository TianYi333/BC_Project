/*
 * FAL (Flash Abstraction Layer) port for the STM32F767 on-chip Flash.
 *
 * This driver replaces the previous "SPI read/write W25Q64JVSSIQ" external flash
 * path. It exposes the tail of the STM32F767ZGT6 internal Flash as a FAL device
 * so that FlashDB / KVDB (and the OTA layer that sits on FAL) can store
 * configuration in the chip's internal Flash instead of an external SPI NOR chip.
 *
 * ===== Direction A: RAM-resident flash primitives (key design point) =====
 * STM32F7 internal Flash programming/erasing STALLS the Flash memory array:
 * while a sector is being erased / a double-word is being programmed, the CPU
 * cannot fetch instructions from that Flash. If the code that drives the flash
 * controller lives in the same Flash bank that is being written, the CPU traps
 * (BusFault -> HardFault) the moment it tries to fetch the next instruction.
 *
 * The robust fix is therefore to run the entire flash-driver critical path from
 * SRAM: the helper routines below are placed in the ".RamFunc" linker section
 * (which is loaded from Flash but executed from RAM, copied by the startup code
 * together with .data). With code executing out of SRAM, the Flash array can be
 * safely stalled without affecting instruction fetch.
 *
 * ===== Layout (dual-bank) =====
 * The chip is run in its factory dual-bank mode:
 *   - Code     : Bank1 (0x08000000..0x0807FFFF), 512KB, the only boot region.
 *   - KVDB     : Bank2 (0x08080000..0x080FFFFF), only the UNIFORM 128KB sectors
 *                S17 (0x080A0000) + S18 (0x080C0000) are used => 256KB, 2 sectors
 *                (FlashDB needs >= 2 erase sectors for wear levelling).
 * Writing Bank2 from code in Bank1 is safe; combined with RAM-resident drivers
 * it is bullet-proof regardless of bank mode.
 * (The FLASH linker length is clamped to 512KB so code can never spill into
 *  Bank2 and collide with the KVDB.)
 */

#include <fal.h>
#include <string.h>
#include "stm32f7xx_hal.h"

#ifndef STM32_FLASH_BASE
#define STM32_FLASH_BASE          ((uint32_t)0x08000000)
#endif

/* Dual-bank KVDB region: Bank2 uniform 128KB sectors S17 + S18. */
#define KVDB_ADDR   ((uint32_t)0x080A0000)   /* Bank2 Sector17 start */
#define KVDB_SIZE   ((uint32_t)(256 * 1024)) /* S17 + S18 = 2 x 128KB */

/* Flash key sequence for unlocking the controller (defined by the HAL). */
#if !defined(FLASH_KEY1)
#define FLASH_KEY1  0x45670123U
#endif
#if !defined(FLASH_KEY2)
#define FLASH_KEY2  0xCDEF89ABU
#endif

/* All error / status flags that are cleared by writing 1 (w1c). */
#define FLASH_ERR_FLAGS  (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                          FLASH_SR_PGPERR | FLASH_SR_ERSERR)

extern void DebugUart_Panic(const char *msg);
extern void DebugUart_PanicHex(const char *label, uint32_t value);

/* Captured FLASH->SR at the last failed RAM primitive (for diagnostics). */
static uint32_t g_flash_last_sr = 0;

/* ---------- RAM-resident low-level flash primitives (no HAL, no libc) ----------
 * Placed in ".RamFunc" so they are executed from SRAM, never from the Flash that
 * is being programmed/erased. */
#define RAMFUNC  __attribute__((section(".RamFunc"))) __attribute__((noinline))

RAMFUNC static void ram_flash_wait(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) { }
}

RAMFUNC static void ram_flash_clear_flags(void)
{
    /* write-1-to-clear all status/error bits */
    FLASH->SR = (FLASH_SR_EOP | FLASH_ERR_FLAGS);
}

RAMFUNC static void ram_flash_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

RAMFUNC static void ram_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* Program one 64-bit double-word. Caller must ensure the target is erased. */
RAMFUNC static int ram_flash_program_dword(uint32_t addr, uint64_t data)
{
    ram_flash_wait();

    FLASH->CR &= CR_PSIZE_MASK;
    FLASH->CR |= FLASH_PSIZE_DOUBLE_WORD;   /* 64-bit programming */

    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)(uint32_t)addr        = (uint32_t)data;
    *(volatile uint32_t *)(uint32_t)(addr + 4U) = (uint32_t)(data >> 32U);
    ram_flash_wait();
    FLASH->CR &= ~FLASH_CR_PG;

    g_flash_last_sr = FLASH->SR;
    /* BSY is already clear on return, so "no error flags" == operation completed.
     * Do NOT rely on EOP (not always latched on this part) to avoid false -1. */
    if ((g_flash_last_sr & FLASH_ERR_FLAGS) == 0U) {
        FLASH->SR = FLASH_SR_EOP;   /* best-effort clear */
        return 0;
    }
    return -1;
}

/* Erase one physical sector identified by its SNB value (FLASH_CR SNB field). */
RAMFUNC static int ram_flash_erase_sector(uint32_t snb)
{
    ram_flash_wait();

    FLASH->CR &= CR_PSIZE_MASK;
    FLASH->CR |= FLASH_PSIZE_DOUBLE_WORD;
    FLASH->CR &= ~FLASH_CR_SNB;            /* clear sector field */
    FLASH->CR |= FLASH_CR_SER | ((snb & 0x1FU) << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    __DSB();
    ram_flash_wait();
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);

    g_flash_last_sr = FLASH->SR;
    /* BSY is already clear on return, so "no error flags" == operation completed.
     * Do NOT rely on EOP (not always latched on this part) to avoid false -1. */
    if ((g_flash_last_sr & FLASH_ERR_FLAGS) == 0U) {
        FLASH->SR = FLASH_SR_EOP;   /* best-effort clear */
        return 0;
    }
    return -1;
}

/* ---------- FAL device plumbing ---------- */

/* Sector table for the KVDB region (filled by init()). */
static struct {
    uint32_t addr;     /* physical sector start */
    uint32_t size;     /* sector size (bytes) */
    uint32_t sector;   /* FLASH_CR SNB value */
} g_sec[2];
static int      g_nsectors = 0;
static uint32_t g_blk_size = 0;   /* erase granularity reported to FAL/FlashDB */

static int init(void);
static int read(long offset, uint8_t *buf, size_t size);
static int write(long offset, const uint8_t *buf, size_t size);
static int erase(long offset, size_t size);

struct fal_flash_dev stm32_onchip = {
    .name      = "stm32_onchip",
    .addr      = KVDB_ADDR,
    .len       = KVDB_SIZE,
    .blk_size  = 128 * 1024,   /* dual-bank uniform 128KB sector */
    .ops       = { init, read, write, erase },
    .write_gran = 64,          /* STM32F7: double-word (64-bit) programming */
};

static int init(void)
{
    /* This layout requires dual-bank mode (Bank1 code / Bank2 KVDB). The chip is
     * configured for dual-bank by default; if it somehow ended up in single-bank
     * (e.g. a full option-byte erase), refuse loudly instead of crashing. */
    uint32_t dual_bank = 0;
#if defined(FLASH_OPTCR_nDBANK)
    dual_bank = ((FLASH->OPTCR & FLASH_OPTCR_nDBANK) == 0U) ? 1U : 0U;
#endif
    if (!dual_bank) {
        DebugUart_Panic("\r\n[FAL] ERROR: single-bank mode detected. "
                        "Set nDBANK=0 (dual-bank) via STM32CubeProgrammer, then retry.\r\n");
        while (1) { }
    }

    stm32_onchip.addr = KVDB_ADDR;
    stm32_onchip.len  = KVDB_SIZE;

    /* Uniform 128KB sectors S17 / S18 in Bank2. */
    g_sec[0].addr   = KVDB_ADDR;
    g_sec[0].size   = 128 * 1024U;
    g_sec[0].sector = 17U;                       /* S17 */
    g_sec[1].addr   = KVDB_ADDR + 128 * 1024U;
    g_sec[1].size   = 128 * 1024U;
    g_sec[1].sector = 18U;                       /* S18 */
    g_nsectors      = 2;
    g_blk_size      = 128 * 1024U;

    stm32_onchip.blk_size = g_blk_size;
    return 0;
}

static int read(long offset, uint8_t *buf, size_t size)
{
    uint32_t addr = stm32_onchip.addr + (uint32_t)offset;

    if (offset < 0 || ((uint32_t)offset + size) > stm32_onchip.len) {
        return -1;
    }
    memcpy(buf, (const void *)addr, size);
    return (int)size;
}

RAMFUNC static int write(long offset, const uint8_t *buf, size_t size)
{
    uint32_t addr = stm32_onchip.addr + (uint32_t)offset;
    uint32_t end  = addr + (uint32_t)size;
    uint32_t primask;
    size_t   i;
    int      ret = 0;

    if (offset < 0 || end > (stm32_onchip.addr + stm32_onchip.len)) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    /* FAL write_gran = 64 bit => 8-byte aligned offset/size required. */
    if (((addr & 0x7U) != 0U) || ((size & 0x7U) != 0U)) {
        return -1;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    ram_flash_unlock();
    ram_flash_clear_flags();

    for (i = 0; i < size; i += 8U) {
        uint64_t d = 0;
        const uint8_t *p = buf + i;
        d  = (uint64_t)p[0];
        d |= (uint64_t)p[1] << 8;
        d |= (uint64_t)p[2] << 16;
        d |= (uint64_t)p[3] << 24;
        d |= (uint64_t)p[4] << 32;
        d |= (uint64_t)p[5] << 40;
        d |= (uint64_t)p[6] << 48;
        d |= (uint64_t)p[7] << 56;

        if (ram_flash_program_dword(addr + (uint32_t)i, d) != 0) {
            ret = -1;
            DebugUart_PanicHex("\r\n[FLASH] prog fail SR=", g_flash_last_sr);
            break;
        }
    }

    ram_flash_lock();
    __set_PRIMASK(primask);
    return ret;
}

RAMFUNC static int erase(long offset, size_t size)
{
    uint32_t addr = stm32_onchip.addr + (uint32_t)offset;
    uint32_t end  = addr + (uint32_t)size;
    int start_idx = -1, end_idx = -1;
    uint32_t primask;
    int ret = 0;

    if (offset < 0 || end > (stm32_onchip.addr + stm32_onchip.len)) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    for (int s = 0; s < g_nsectors; s++) {
        uint32_t s_end = g_sec[s].addr + g_sec[s].size;
        if (addr >= g_sec[s].addr && addr < s_end && start_idx < 0) {
            start_idx = s;
        }
        if (end > g_sec[s].addr && end <= s_end) {
            end_idx = s;
        }
    }
    if (start_idx < 0 || end_idx < 0 || end_idx < start_idx) {
        return -1;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    ram_flash_unlock();
    ram_flash_clear_flags();

    for (int s = start_idx; s <= end_idx; s++) {
        if (ram_flash_erase_sector(g_sec[s].sector) != 0) {
            ret = -1;
            DebugUart_PanicHex("\r\n[FLASH] erase fail SR=", g_flash_last_sr);
            break;
        }
    }

    ram_flash_lock();
    __set_PRIMASK(primask);
    return ret;
}
