/*
 * @file           : main_boot.c
 * @brief          : STM32F767 独立 Bootloader（OTA 烧录，无外部 Flash / 无备份）
 *
 * 布局（双 Bank，F767ZGT6 = 1MB Flash，G=1MB，不是 2MB！）：
 *   Bank1 = 512KB @0x08000000（S0~S3=16K, S4=64K, S5~S7=128K）
 *   Bank2 = 512KB @0x08080000（S12~S15=16K, S16=64K, S17~S19=128K）
 *   扇区号不连续（AN4826 图1）；Bank2 的 FLASH_CR.SNB 寄存器值 = 扇区号 + 4。
 *   - Bootloader : Bank1 S0~S4   (0x08000000) 128KB，复位入口，永不改
 *   - firmware_a : Bank1 S5~S7   (0x08020000) 384KB，App 端 TFTP 收的新固件
 *   - ef_kvdb1   : Bank2 S12~S15 (0x08080000)  64KB，配置 KV（Boot 不碰）
 *   - ota_meta   : Bank2 S16     (0x08090000)  64KB，状态字
 *   - App        : Bank2 S17~S19 (0x080A0000) 384KB，由本 Boot 烧录
 *
 * 流程（复用 D:\Desktop\boot 的 H7 状态机语义，但落在本芯片内部 Flash 上）：
 *   - 读 ota_meta
 *     * PENDING : 校验 firmware_a CRC -> 擦 Bank2 + 烧 firmware_a -> 校验 Bank2
 *                 -> 置 DONE；失败置 FAILED。全部拒绝越界/同 Bank 写，绝不以
 *                 坏固件覆盖 Bank2 的有效 App（防砖核心）。
 *     * DONE    : 上次烧了但 App 未确认（起不来）-> 无备份，直接再跳 App 给一次机会。
 *     * CONFIRMED: 升级完成 -> 清 NONE。
 *     * NONE/FAIL/默认: 直接跳 App。
 *   - 校验 App 向量表(SP/PC)合法性后跳转。
 *
 * 关键安全点（与 App 的 .RamFunc 双字编程同源）：
 *   - 写 Flash 的原语放在 .RamFunc（SRAM 执行），规避"写 Flash 期间 CPU 取指同
 *     Bank 崩溃"。Boot 在 Bank1：烧 firmware_a 属同 Bank；写 meta/App 属跨 Bank，
 *     统一走 RamFunc，保持一致。
 *   - 双字编程拆两次独立字编程，规避 SRAM 过快触发 PGPERR（见 App fal 驱动注释）。
 *   - 单 Bank(nDBANK=1) 时，Bank2 写法会变同 Bank，极危险：此时禁止一切写 Flash，
 *     仅跳现有 App（请先用 CubeProgrammer 设 nDBANK=0）。
 */
#include "stm32f767xx.h"
#include <string.h>
#include "ota_common.h"
#include "boot_crc.h"
#include "boot_uart.h"

/* ---------- 物理地址/容量（与 ota_common.h / STM32F767xx_FLASH.ld 严格对齐） ---------- */
#define APP_ADDRESS       ((uint32_t)OTA_APP_ADDRESS)     /* 0x080A0000 */
#define APP_STACK_TOP     (*((volatile uint32_t *)APP_ADDRESS))
#define APP_RESET_HANDLER (*((volatile uint32_t *)(APP_ADDRESS + 4)))
#define APP_FLASH_SIZE    ((uint32_t)OTA_APP_MAX_SIZE)    /* 384KB */

/* Flash 解锁密钥（CMSIS 头未定义，照搬 App fal 驱动） */
#ifndef FLASH_KEY1
#define FLASH_KEY1  0x45670123U
#endif
#ifndef FLASH_KEY2
#define FLASH_KEY2  0xCDEF89ABU
#endif

/* 所有写 1 清的错误标志 */
#define FLASH_ERR_FLAGS  (FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                          FLASH_SR_PGPERR | FLASH_SR_ERSERR)

/* PSIZE 取值（FLASH_CR[9:8]） */
#define FL_PSIZE_WORD  (0x2UL << FLASH_CR_PSIZE_Pos)   /* 32-bit */
#define FL_PSIZE_DWORD (0x3UL << FLASH_CR_PSIZE_Pos)   /* 64-bit */

/* ---------- RAM 驻留 Flash 原语（.RamFunc，SRAM 执行） ---------- */
#define RAMFUNC  __attribute__((section(".RamFunc"))) __attribute__((noinline))

RAMFUNC static void bf_wait(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) { }
}

RAMFUNC static void bf_clear_flags(void)
{
    FLASH->SR = (FLASH_SR_EOP | FLASH_ERR_FLAGS);
}

RAMFUNC static void bf_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

RAMFUNC static void bf_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* 编程一个 32-bit 字（目标 4 字节须已擦除为 0xFFFFFFFF） */
RAMFUNC static int bf_prog_word(uint32_t addr, uint32_t data)
{
    bf_wait();
    bf_clear_flags();

    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FL_PSIZE_WORD;     /* 32-bit 编程 */
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)addr = data;
    __DSB();
    bf_wait();
    FLASH->CR &= ~FLASH_CR_PG;

    if ((FLASH->SR & FLASH_ERR_FLAGS) == 0U) {
        FLASH->SR = FLASH_SR_EOP;
        return 0;
    }
    return -1;
}

/* 双字编程拆两次独立字编程（规避 SRAM 过快触发 PGPERR，与 App 驱动一致） */
RAMFUNC static int bf_prog_dword(uint32_t addr, uint64_t data)
{
    int r = bf_prog_word(addr, (uint32_t)data);
    if (r == 0) {
        r = bf_prog_word(addr + 4U, (uint32_t)(data >> 32U));
    }
    return r;
}

/* 擦除一个物理扇区（SNB = FLASH_CR[7:3]） */
RAMFUNC static int bf_erase_sector(uint32_t snb)
{
    bf_wait();
    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FL_PSIZE_DWORD;    /* erase 时 PSIZE 取值无关，置 DOUBLE_WORD */
    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= FLASH_CR_SER | ((snb & 0x1FU) << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    __DSB();
    bf_wait();
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);

    if ((FLASH->SR & FLASH_ERR_FLAGS) == 0U) {
        FLASH->SR = FLASH_SR_EOP;
        return 0;
    }
    return -1;
}

/* IWDG 喂狗（若 App 以硬件模式启动了 IWDG，长烧录期间需喂；写 0xAAAA 不启用 IWDG，安全） */
static void boot_kick_iwdg(void)
{
    IWDG->KR = 0xAAAAU;
}

/* ---------------- 日志辅助（全部经 boot_printf 轮询发出） ---------------- */
static const char *state_str(uint32_t st)
{
    switch (st) {
    case OTA_STATE_NONE:      return "NONE(0)";
    case OTA_STATE_PENDING:   return "PENDING(1)";
    case OTA_STATE_DONE:      return "DONE(2)";
    case OTA_STATE_CONFIRMED: return "CONFIRMED(3)";
    case OTA_STATE_FAILED:    return "FAILED(4)";
    default:                  return "UNKNOWN";
    }
}

/* 复位原因（只读不清，避免影响 App 侧判断） */
static void print_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    boot_printf("[BOOT] RST CSR=0x%08X ->", (unsigned long)csr);
    if ((csr & RCC_CSR_LPWRRSTF)  != 0U) boot_printf(" LPWR");
    if ((csr & RCC_CSR_WWDGRSTF)  != 0U) boot_printf(" WWDG");
    if ((csr & RCC_CSR_IWDGRSTF)  != 0U) boot_printf(" IWDG");
    if ((csr & RCC_CSR_SFTRSTF)   != 0U) boot_printf(" SFT");
    if ((csr & RCC_CSR_PORRSTF)   != 0U) boot_printf(" POR");
    if ((csr & RCC_CSR_PINRSTF)   != 0U) boot_printf(" PIN");
    boot_printf("\n");
}

/* ---------- 高层操作（普通 Flash 代码，仅调用 RamFunc 原语） ---------- */

static uint32_t calc_crc(uint32_t addr, uint32_t len)
{
    return boot_calc_crc32(0, (const void *)addr, len);
}

/* 把 ota_meta 写回（Bank2 S16，相对 Boot=Bank1 为跨 Bank；统一 RamFunc + 关中断） */
static int write_meta(const ota_meta_t *meta)
{
    uint32_t primask = __get_PRIMASK();
    int ret = 0;

    __disable_irq();
    bf_unlock();
    bf_clear_flags();

    boot_printf("[BOOT] write meta: erase S16(snb20) @0x%08X\n", (unsigned long)OTA_META_OFFSET);
    /* meta 落在 Bank2 S16（0x08090000）；双 Bank 下 SNB 寄存器值 = 扇区号 16 + 4 = 20 */
    if (bf_erase_sector(20) != 0) {
        boot_dump_flash_sr("meta erase fail", FLASH->SR);
        ret = -1;
        goto done;
    }
    {
        const uint8_t *p = (const uint8_t *)meta;
        uint32_t a = OTA_META_OFFSET;
        for (uint32_t i = 0; i < sizeof(ota_meta_t); i += 8U) {
            uint64_t d = 0xFFFFFFFFFFFFFFFFULL;
            uint32_t chunk = (sizeof(ota_meta_t) - i >= 8U) ? 8U : (sizeof(ota_meta_t) - i);
            for (uint32_t j = 0; j < chunk; j++) {
                d = (d & ~((uint64_t)0xFFU << (8U * j))) | ((uint64_t)p[i + j] << (8U * j));
            }
            if (bf_prog_dword(a + i, d) != 0) {
                boot_printf("[BOOT] meta prog fail @0x%08X\n", (unsigned long)(a + i));
                boot_dump_flash_sr("meta prog", FLASH->SR);
                ret = -1;
                goto done;
            }
        }
    }
done:
    bf_lock();
    __set_PRIMASK(primask);
    if (ret == 0) {
        boot_printf("[BOOT] write meta OK -> state=%s len=%u crc=0x%08X\n",
                    state_str(meta->state),
                    (unsigned long)meta->new_len,
                    (unsigned long)meta->new_crc32);
    } else {
        boot_printf("[BOOT] write meta FAILED (state=%s)\n", state_str(meta->state));
    }
    return ret;
}

/* 擦 Bank2 App 区(S17~S19)并把 firmware_a 烧入 App 区（跨 Bank 写，天然安全） */
static int burn_app(uint32_t len)
{
    if (len == 0 || len > APP_FLASH_SIZE) {
        boot_printf("[BOOT] ERR burn_app: bad len=%u (max %u)\n",
                    (unsigned long)len, (unsigned long)APP_FLASH_SIZE);
        return -1;
    }
    /* App 区 = Bank2 S17~S19 @ 0x080A0000~0x080FFFFF（384KB = 3×128K）。
     * 双 Bank 下 Bank2 的 FLASH_CR.SNB 寄存器值 = 扇区号 + 4（见 HAL
     * FLASH_Erase_Sector 的 "+4" 规则）：S17~S19 -> 21~23。
     * 统一擦整段 App 区（恒在 App 范围内，不越界、不碰 Bootloader/ota_meta/KVDB）。 */
    static const uint8_t app_snb[3] = {21, 22, 23};

    uint32_t primask = __get_PRIMASK();
    int ret = 0;

    __disable_irq();
    bf_unlock();
    bf_clear_flags();

    for (int i = 0; i < 3; i++) {
        boot_printf("[BOOT] erase App S%u (snb=%u) ... ",
                    (unsigned long)(app_snb[i] - 4U), (unsigned long)app_snb[i]);
        if (bf_erase_sector((uint32_t)app_snb[i]) != 0) {
            boot_printf("FAIL\n");
            boot_dump_flash_sr("erase", FLASH->SR);
            ret = -1;
            goto done;
        }
        boot_printf("ok\n");
    }

    {
        uint32_t src = OTA_FW_A_OFFSET;
        uint32_t dst = APP_ADDRESS;
        uint32_t remaining = len;
        uint32_t done = 0;
        uint32_t next_pct = 0;
        boot_printf("[BOOT] burn %uB: fw_a 0x%08X -> App 0x%08X\n",
                    (unsigned long)len, (unsigned long)src, (unsigned long)dst);
        while (remaining > 0) {
            uint64_t d = 0xFFFFFFFFFFFFFFFFULL;
            uint32_t chunk = (remaining >= 8U) ? 8U : remaining;
            const uint8_t *sp = (const uint8_t *)src;
            for (uint32_t j = 0; j < chunk; j++) {
                d = (d & ~((uint64_t)0xFFU << (8U * j))) | ((uint64_t)sp[j] << (8U * j));
            }
            if (bf_prog_dword(dst, d) != 0) {
                boot_printf("[BOOT] ERR prog fail @0x%08X\n", (unsigned long)dst);
                boot_dump_flash_sr("prog", FLASH->SR);
                ret = -1;
                goto done;
            }
            src += 8U;
            dst += 8U;
            remaining -= chunk;
            done += chunk;
            if (done >= next_pct) {
                boot_printf("[BOOT]   burn %u%% (%u/%u)\n",
                            (unsigned long)((done * 100UL) / len),
                            (unsigned long)done, (unsigned long)len);
                next_pct += (len / 4U) + 1U;      /* 约每 25% 一行 */
            }
            boot_kick_iwdg();
        }
        boot_printf("[BOOT] burn 100%% done (%u/%u)\n", (unsigned long)done, (unsigned long)len);
    }
done:
    bf_lock();
    __set_PRIMASK(primask);
    return ret;
}

static void do_update(ota_meta_t *meta)
{
    uint32_t crc;

    /* 1. 校验 firmware_a 完整性（CRC 与上位机一致） */
    boot_printf("[BOOT] step1: verify fw_a CRC (len=%u, exp=0x%08X)\n",
                (unsigned long)meta->new_len, (unsigned long)meta->new_crc32);
    crc = calc_crc(OTA_FW_A_OFFSET, meta->new_len);
    if (crc != meta->new_crc32) {
        boot_printf("[BOOT] ERR fw_a CRC mismatch act=0x%08X exp=0x%08X -> FAILED\n",
                    (unsigned long)crc, (unsigned long)meta->new_crc32);
        meta->state = OTA_STATE_FAILED;
        write_meta(meta);
        return;
    }
    boot_printf("[BOOT] fw_a CRC OK (0x%08X)\n", (unsigned long)crc);

    /* 2. 擦 Bank2 + 烧 firmware_a */
    boot_printf("[BOOT] step2: erase App Bank2 + burn\n");
    if (burn_app(meta->new_len) != 0) {
        boot_printf("[BOOT] ERR burn_app failed -> FAILED\n");
        meta->state = OTA_STATE_FAILED;
        write_meta(meta);
        return;
    }

    /* 3. 校验 Bank2 CRC（失败重试一次，仍失败则标记 FAILED，保留现有 App 跳转） */
    boot_printf("[BOOT] step3: verify App CRC\n");
    crc = calc_crc(APP_ADDRESS, meta->new_len);
    if (crc != meta->new_crc32) {
        boot_printf("[BOOT] WARN App CRC mismatch act=0x%08X exp=0x%08X -> retry burn\n",
                    (unsigned long)crc, (unsigned long)meta->new_crc32);
        if (burn_app(meta->new_len) != 0) {
            boot_printf("[BOOT] ERR retry burn failed -> FAILED\n");
            meta->state = OTA_STATE_FAILED;
            write_meta(meta);
            return;
        }
        crc = calc_crc(APP_ADDRESS, meta->new_len);
        if (crc != meta->new_crc32) {
            boot_printf("[BOOT] ERR App CRC still mismatch act=0x%08X exp=0x%08X -> FAILED\n",
                        (unsigned long)crc, (unsigned long)meta->new_crc32);
            meta->state = OTA_STATE_FAILED;
            write_meta(meta);
            return;
        }
    }
    boot_printf("[BOOT] App CRC OK (0x%08X)\n", (unsigned long)crc);

    meta->state = OTA_STATE_DONE;
    write_meta(meta);
}

/* ---------- 跳转 ---------- */
static int app_valid(void)
{
    uint32_t sp = APP_STACK_TOP;
    uint32_t pc = APP_RESET_HANDLER;
    /* SP 须在 SRAM 范围(0x20000000~0x20080000) 且非 0xFFFFFFFF；
     * 初始 MSP 通常等于 RAM 顶 0x20080000（首次压栈才减），故上界取 <=。
     * PC 须在 Bank2 App 范围且 Thumb 位(bit0)置位。 */
    return (sp >= 0x20000000UL && sp <= 0x20080000UL && sp != 0xFFFFFFFFUL) &&
           (pc >= APP_ADDRESS && pc < (APP_ADDRESS + APP_FLASH_SIZE) && (pc & 0x1U) != 0U);
}

static void JumpToApp(void)
{
    __disable_irq();
    __set_MSP(APP_STACK_TOP);
    SCB->VTOR = APP_ADDRESS;        /* 防御性：确保 App 向量表指向 Bank2 */
    ((void (*)(void))APP_RESET_HANDLER)();
}

int main(void)
{
    /* 双 Bank 检查：单 Bank 时写 Bank2 会变成同 Bank 写，极危险，禁止一切写 Flash。 */
    uint32_t dual_bank = 0;

    boot_uart_init();
    boot_printf("\n[BOOT] == SensorConcentrator Bootloader (F767ZGT6 1MB, dual-bank) ==\n");
    boot_printf("[BOOT] uart: DEBUG-CH1 UART7 PF7/DE=PA5 115200 8N1 (PCLK1=%uHz)\n",
                (unsigned long)16000000UL);
    print_reset_cause();

    if ((FLASH->OPTCR & FLASH_OPTCR_nDBANK) == 0U) {
        dual_bank = 1U;
    }
    boot_printf("[BOOT] nDBANK=%u -> %s\n",
                (unsigned long)((FLASH->OPTCR & FLASH_OPTCR_nDBANK) ? 1UL : 0UL),
                dual_bank ? "dual-bank (OK)" : "SINGLE-bank");

    ota_meta_t meta;
    memcpy(&meta, (const void *)OTA_META_OFFSET, sizeof(meta));

    if (meta.magic != OTA_META_MAGIC) {
        boot_printf("[BOOT] meta: no magic (0x%08X), skip OTA\n", (unsigned long)meta.magic);
    } else {
        boot_printf("[BOOT] meta: magic=OK state=%s len=%u crc=0x%08X\n",
                    state_str(meta.state),
                    (unsigned long)meta.new_len,
                    (unsigned long)meta.new_crc32);
    }

    if (meta.magic == OTA_META_MAGIC && dual_bank) {
        switch (meta.state) {
        case OTA_STATE_PENDING:
            boot_printf("[BOOT] -> PENDING, start update\n");
            do_update(&meta);
            break;
        case OTA_STATE_DONE:
            /* 烧了新固件但 App 未确认（起不来）。无备份：直接再跳 App 给一次机会。 */
            boot_printf("[BOOT] -> DONE but App not confirmed yet, retry jump\n");
            break;
        case OTA_STATE_CONFIRMED:
            boot_printf("[BOOT] -> CONFIRMED, clear to NONE\n");
            meta.state = OTA_STATE_NONE;
            write_meta(&meta);
            break;
        case OTA_STATE_NONE:
            boot_printf("[BOOT] -> NONE, no update\n");
            break;
        case OTA_STATE_FAILED:
            boot_printf("[BOOT] -> FAILED (last update failed), boot existing App\n");
            break;
        default:
            boot_printf("[BOOT] -> unknown state=%u, sanitize to NONE & boot\n",
                        (unsigned long)meta.state);
            meta.state = OTA_STATE_NONE;
            meta.new_len = 0;
            meta.new_crc32 = 0;
            write_meta(&meta);
            break;
        }
    } else if (meta.magic == OTA_META_MAGIC && !dual_bank) {
        boot_printf("[BOOT] ERR single-bank: refuse all flash writes, jump App only\n");
    }

    if (app_valid()) {
        boot_printf("[BOOT] app valid: SP=0x%08X PC=0x%08X -> JUMP\n",
                    (unsigned long)APP_STACK_TOP, (unsigned long)APP_RESET_HANDLER);
        JumpToApp();
    }

    boot_printf("[BOOT] ERR no valid App (SP=0x%08X PC=0x%08X) -> HALT\n",
                (unsigned long)APP_STACK_TOP, (unsigned long)APP_RESET_HANDLER);
    while (1) {
        __NOP();
    }
}
