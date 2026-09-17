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
 * ===== Layout (dual-bank, F767ZG = 1MB total) =====
 * ⚠️ STM32F767ZGT6 is a 1MB part (G=1MB, NOT 2MB). Dual-bank (nDBANK=0):
 *    Bank1 = 512KB @0x08000000 (S0~S3=16K, S4=64K, S5~S7=128K),
 *    Bank2 = 512KB @0x08080000 (S12~S15=16K, S16=64K, S17~S19=128K).
 *    Sector numbers are non-continuous (AN4826 Fig.1: 16 sectors total); Bank2's
 *    FLASH_CR.SNB register value = sector number + 4 (HAL FLASH_Erase_Sector
 *    "+4" rule) -> 16~23.
 *   - Bootloader: Bank1 S0~S4    0x08000000 128KB, reset entry, never changed
 *   - firmware_a: Bank1 S5~S7    0x08020000 384KB, TFTP staging of new firmware
 *   - ef_kvdb1  : Bank2 S12~S15  0x08080000  64KB, config KV (4x16KB wear leveling)
 *   - ota_meta  : Bank2 S16      0x08090000  64KB, OTA status word
 *   - App       : Bank2 S17~S19  0x080A0000 384KB (see STM32F767xx_FLASH.ld)
 * App runs in Bank2: writing firmware_a (Bank1) is cross-bank (RWW, no stall);
 * writing KVDB/meta (Bank2) is same-bank -> done via .RamFunc SRAM-resident
 * primitives (see RAMFUNC primitives below).
 */

#include <fal.h>
#include <string.h>
#include "stm32f7xx_hal.h"

#ifndef STM32_FLASH_BASE
#define STM32_FLASH_BASE          ((uint32_t)0x08000000)
#endif

/* 内部 Flash OTA 设备区：0x08020000 ~ 0x080FFFFF（896KB）=
 *   Bank1 扇区 S5~S7（3x128KB） + Bank2 扇区 S12~S19（4x16K + 64K + 3x128KB）。
 * FLASH_CR.SNB 寄存器值：Bank1 S5~S7 -> 5~7（等于扇区号）；
 *   Bank2 S12~S19 -> 16~23（= 扇区号 + 4，见 HAL "+4" 规则）。
 * 该区含：firmware_a(0x08020000,S5~S7,384KB) + ef_kvdb1(0x08080000,S12~S15,64KB)
 *         + ota_meta(0x08090000,S16,64KB)。App 已重定位到 Bank2 0x080A0000。 */
#define OTA_DEV_ADDR   ((uint32_t)0x08020000)   /* 设备起点 = firmware_a (S5) */
#define OTA_DEV_SIZE   ((uint32_t)(896 * 1024)) /* 0x08020000 ~ 0x080FFFFF */

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

/* Program one 32-bit word. Caller must ensure the 4-byte target is erased (0xFFFFFFFF). */
RAMFUNC static int ram_flash_program_word(uint32_t addr, uint32_t data)
{
    ram_flash_wait();
    ram_flash_clear_flags();

    FLASH->CR &= CR_PSIZE_MASK;
    FLASH->CR |= FLASH_PSIZE_WORD;   /* 32-bit programming */
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)addr = data;
    __DSB();
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

/* Program one 64-bit double-word by issuing two independent 32-bit word programs.
 *
 * Why split instead of one double-word store sequence?
 * Running this flash driver from SRAM (RamFunc) issues the two back-to-back
 * 32-bit stores of a double-word program faster than the F7 flash controller
 * can latch the low half, which raises PGPERR (Programming Parallelism Error,
 * FLASH_SR bit 6). The ST HAL avoids this only because it executes from the
 * (slower) on-chip Flash, not SRAM. Issuing two separate word programs, each
 * with its own BSY wait, removes the timing hazard entirely while staying
 * functionally identical to a single double-word write. Caller must ensure both
 * 4-byte halves are erased (each 0xFFFFFFFF). */
RAMFUNC static int ram_flash_program_dword(uint32_t addr, uint64_t data)
{
    int r = ram_flash_program_word(addr, (uint32_t)data);
    if (r == 0) {
        r = ram_flash_program_word(addr + 4U, (uint32_t)(data >> 32U));
    }
    return r;
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
} g_sec[11];
static int      g_nsectors = 0;
static uint32_t g_blk_size = 0;   /* erase granularity reported to FAL/FlashDB */

static int init(void);
static int read(long offset, uint8_t *buf, size_t size);
static int write(long offset, const uint8_t *buf, size_t size);
static int erase(long offset, size_t size);

struct fal_flash_dev stm32_onchip = {
    .name      = "stm32_onchip",
    .addr      = OTA_DEV_ADDR,
    .len       = OTA_DEV_SIZE,
    .blk_size  = 16 * 1024,    /* 最小擦除粒度（Bank2 S12~S15 的 16KB 扇区） */
    .ops       = { init, read, write, erase },
    .write_gran = 64,          /* STM32F7: double-word (64-bit) programming */
};

static int init(void)
{
    /* This layout requires dual-bank mode (Boot=Bank1, App/KVDB/meta=Bank2). The chip is
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

    stm32_onchip.addr = OTA_DEV_ADDR;
    stm32_onchip.len  = OTA_DEV_SIZE;

    /* 非均匀扇区表（F767ZG 1MB 双 Bank，AN4826 图1）：
     * Bank1 S5~S7(128K) = firmware_a；Bank2 S12~S15(16K) = ef_kvdb1；
     * Bank2 S16(64K) = ota_meta；Bank2 S17~S19(128K) = App 区（本设备也一并纳管）。
     * Bank1 的 SNB 寄存器值 == 扇区号本身；Bank2 的 SNB 寄存器值 = 扇区号 + 4。 */
    static const struct {
        uint32_t addr;
        uint32_t size;
        uint32_t snb;
    } tbl[11] = {
        {0x08020000UL, 128U * 1024U, 5U},   /* Bank1 S5              */
        {0x08040000UL, 128U * 1024U, 6U},   /* Bank1 S6              */
        {0x08060000UL, 128U * 1024U, 7U},   /* Bank1 S7              */
        {0x08080000UL,  16U * 1024U, 16U},  /* Bank2 S12 (SNB=12+4)  */
        {0x08084000UL,  16U * 1024U, 17U},  /* Bank2 S13 (SNB=13+4)  */
        {0x08088000UL,  16U * 1024U, 18U},  /* Bank2 S14 (SNB=14+4)  */
        {0x0808C000UL,  16U * 1024U, 19U},  /* Bank2 S15 (SNB=15+4)  */
        {0x08090000UL,  64U * 1024U, 20U},  /* Bank2 S16 (SNB=16+4)  */
        {0x080A0000UL, 128U * 1024U, 21U},  /* Bank2 S17 (SNB=17+4)  */
        {0x080C0000UL, 128U * 1024U, 22U},  /* Bank2 S18 (SNB=18+4)  */
        {0x080E0000UL, 128U * 1024U, 23U},  /* Bank2 S19 (SNB=19+4)  */
    };
    for (int s = 0; s < 11; s++) {
        g_sec[s].sector = tbl[s].snb;           /* FLASH_CR.SNB 寄存器值 */
        g_sec[s].size   = tbl[s].size;
        g_sec[s].addr   = tbl[s].addr;
    }
    g_nsectors      = 11;
    g_blk_size      = 16U * 1024U;

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

    /* FAL 契约（见 fal_partition.c 的 "@return >= 0: successful write data size"）：
     * 成功必须返回实际写入的字节数，失败返回负数。
     * 本函数原先统一返回 0，导致按 "wr != len" 判定的调用方（ota.c 的
     * ota_feed_chunk、fal_rtt.c 等）把成功误判为写失败 —— 这正是 TFTP
     * "ERROR 2: error writing file" 的根因。FlashDB 只判 < 0，不受影响。 */
    return (ret == 0) ? (int)size : ret;
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
