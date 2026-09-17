/*
 * @file           : ota.c
 * @brief          : App 端 OTA 接收 + 启动确认（防砖链路第二部分）
 *
 * 与 boot(main_boot.c) 配合完成 A/B 防砖升级：
 *   1) 网关下发 upgrade_start{new_len,new_crc32} → 本模块擦 firmware_a、启动
 *      lwIP TFTP server（UDP/69）接收固件；
 *   2) 传输完成且长度/CRC 校验通过 → 写 ota_meta(PENDING) → 软复位；
 *   3) 下次上电，boot 见 PENDING 烧录内部 Flash 并置 DONE，再跳 App；
 *   4) App 启动成功 → ota_app_confirm() 把 DONE 改写为 CONFIRMED（防砖确认点）。
 *
 * 仅保留 TFTP 一种传输方式（TCP 路径已移除）。
 * 注意：所有 SPI Flash 访问(fal_partition_*) 用现有 flash_kv_mutex 串行化，
 *       避免与 FlashDB/KV 写并发导致 SPI 总线竞争。
 */
#include "ota.h"
#include "project_config.h"

#include <string.h>
#include "stm32f7xx_hal.h"
#include "cmsis_os.h"
#include "semphr.h"
#include "fal.h"
#include "flashdb.h"
#include "lwip/apps/tftp_server.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

/* 复用现有 flash_kv_mutex 串行化 SPI Flash 访问（定义于 freertos.c） */
extern SemaphoreHandle_t flash_kv_mutex;

/* LOG_OTA 由 project_config.h 根据 OTA_DEBUG 开关统一定义 */

/* ---------------- 内部状态 ---------------- */
static ota_rx_state_t g_ota_state = OTA_RX_IDLE;
static uint32_t       g_ota_exp_len = 0;   /* 期望固件字节数 */
static uint32_t       g_ota_exp_crc = 0;   /* 期望固件 CRC32 */
static uint32_t       g_ota_off     = 0;   /* 已写入 firmware_a 的字节数 */
static uint32_t       g_ota_crc     = 0;   /* 增量 CRC32（初始 0，与 boot 端一致） */

static const struct fal_partition *g_fw_part   = NULL;
static const struct fal_partition *g_meta_part = NULL;

/* 暂存缓冲：容纳 TFTP 单块(512) */
static uint8_t g_stage[1536];

/* ---------------- 内部函数前向声明 ---------------- */
static int  ota_feed_chunk(const uint8_t *data, uint16_t len);
static void ota_reset_session(void);
static void ota_on_complete(void);

/* ---------------- 共享：写入一帧 / 收尾 / 复位 ---------------- */

/* 写入一帧数据到 firmware_a（内部越界保护 + mutex + 增量 CRC）。
 * 成功返回 0；越界或写入失败返回 <0。调用方需先把数据拷到 data 指向的缓冲。 */
static int ota_feed_chunk(const uint8_t *data, uint16_t len)
{
    if (g_ota_state != OTA_RX_ACTIVE)
    {
        return -1;
    }
    if (len == 0)
    {
        return 0;
    }
    /* 越界保护：防止畸形/超长数据破坏分区 */
    if ((uint32_t)(g_ota_off + len) > g_ota_exp_len)
    {
        LOG_OTA("ota: chunk overflow off=%lu len=%u exp=%lu\r\n",
                (unsigned long)g_ota_off, (unsigned)len, (unsigned long)g_ota_exp_len);
        return -1;
    }

    /* 底层 stm32_onchip 设备 write_gran = 64bit（8 字节），而 TFTP 末块长度通常
     * 不是 8 的整数倍（实测 203108B 固件的末块 = 356B，356 % 8 = 4），直接写会
     * 被驱动的对齐检查拒绝并返回 -1。
     * 这里把末块尾部补成擦除值 0xFF 对齐到 8 字节：补齐字节落在已擦除区内、
     * 不覆盖任何有效数据。CRC 与写指针仍按补齐前的真实 len 计算/推进。 */
    uint32_t wr_len = len;
    if ((wr_len & 7U) != 0U)
    {
        wr_len = (wr_len + 8U) & ~7U;
        if (wr_len > sizeof(g_stage) ||
            (uint32_t)(g_ota_off + wr_len) > OTA_FW_PART_SIZE)
        {
            LOG_OTA("ota: tail pad overflow off=%lu len=%u\r\n",
                    (unsigned long)g_ota_off, (unsigned)len);
            return -1;
        }
        memset((void *)(data + len), 0xFF, wr_len - len);
    }

    /* 写入 firmware_a 分区（内部 Flash），flash_kv_mutex 串行化 */
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreTakeRecursive(flash_kv_mutex, portMAX_DELAY);
    }
    int wr = fal_partition_write(g_fw_part, g_ota_off, data, wr_len);
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }
    if (wr != (int)wr_len)
    {
        LOG_OTA("ota: fal write fail off=%lu wr=%d\r\n",
                (unsigned long)g_ota_off, (int)wr);
        return -1;
    }

    /* 增量 CRC32（初始 0），与 boot 端 calc_spi_crc 字节级一致 */
    /* 注意：必须用补齐前的真实长度 len —— 补上的 0xFF 不参与 CRC */
    g_ota_crc = fdb_calc_crc32(g_ota_crc, data, len);
    g_ota_off += len;
    return 0;
}

/* 回退到空闲并停止 TFTP 传输服务，允许重试 */
static void ota_reset_session(void)
{
    g_ota_state = OTA_RX_IDLE;
    g_ota_off   = 0;
    g_ota_crc   = 0;

    /* 停止 TFTP 服务释放 UDP pcb */
    tftp_cleanup();
}

/* 传输结束（收满或连接关闭）时调用：校验并落盘/复位，或回 IDLE 允许重试 */
static void ota_on_complete(void)
{
    if (g_ota_state != OTA_RX_ACTIVE)
    {
        return;
    }

    int complete = (g_ota_off == g_ota_exp_len) && (g_ota_crc == g_ota_exp_crc);

    if (!complete)
    {
        LOG_OTA("ota: abort off=%lu exp=%lu crc=%08lX exp=%08lX\r\n",
                (unsigned long)g_ota_off, (unsigned long)g_ota_exp_len,
                (unsigned long)g_ota_crc, (unsigned long)g_ota_exp_crc);
        ota_reset_session();
        return;
    }

    /* 成功：写 ota_meta(PENDING) → 立即软复位，交给定 boot 烧录 */
    LOG_OTA("ota: complete len=%lu crc=%08lX, write meta & reboot\r\n",
            (unsigned long)g_ota_off, (unsigned long)g_ota_crc);

    ota_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic     = OTA_META_MAGIC;
    meta.state     = OTA_STATE_PENDING;
    meta.new_len   = g_ota_exp_len;
    meta.new_crc32 = g_ota_exp_crc;

    if (flash_kv_mutex != NULL)
    {
        xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
    }
    int er = fal_partition_erase(g_meta_part, 0, OTA_META_SIZE);
    int wr = fal_partition_write(g_meta_part, 0, (const uint8_t *)&meta, sizeof(meta));
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }
    /* 判错日志：meta 落盘失败若被静默吞掉，S16 可能停在脏状态，排查无据可依 */
    if (er != (int)OTA_META_SIZE) {
        LOG_OTA("ota: meta erase fail ret=%d\r\n", er);
    }
    if (wr != (int)sizeof(meta)) {
        LOG_OTA("ota: meta write fail ret=%d\r\n", wr);
    }

    g_ota_state = OTA_RX_DONE;

    /* 立即复位，boot 将读取 PENDING 并烧录内部 Flash */
    HAL_NVIC_SystemReset();
    /* 不会返回 */
}

/* ---------------- TFTP server 回调 ---------------- */

static void *tftp_open_cb(const char *fname, const char *mode, u8_t write)
{
    LWIP_UNUSED_ARG(mode);

    /* 仅接受写请求（网关 tftp put），且限定文件名为 OTA_TFTP_FILENAME */
    if (write == 0)
    {
        LOG_OTA("ota_tftp: read not supported\r\n");
        return NULL;
    }
    if (strcmp(fname, OTA_TFTP_FILENAME) != 0)
    {
        LOG_OTA("ota_tftp: reject file %s (expect %s)\r\n", fname, OTA_TFTP_FILENAME);
        return NULL;
    }
    if (g_ota_state != OTA_RX_ACTIVE)
    {
        LOG_OTA("ota_tftp: no active upgrade session\r\n");
        return NULL;
    }
    /* 返回非空句柄即可（本模块用全局状态跟踪） */
    return (void *)1;
}

/* 本机仅作接收方，不支持读；提供空实现避免 NULL 函数指针告警 */
static int tftp_read_cb(void *handle, void *buf, int bytes)
{
    LWIP_UNUSED_ARG(handle);
    LWIP_UNUSED_ARG(buf);
    LWIP_UNUSED_ARG(bytes);
    return -1;
}

static int tftp_write_cb(void *handle, struct pbuf *p)
{
    LWIP_UNUSED_ARG(handle);

    if (g_ota_state != OTA_RX_ACTIVE)
    {
        return -1;
    }
    uint16_t len = p->tot_len;
    if (len == 0)
    {
        return 0;
    }
    /* 拷贝出 pbuf 数据：回调返回后 pbuf 可能被释放/复用 */
    if (pbuf_copy_partial(p, g_stage, len, 0) != len)
    {
        return -1;
    }
    return ota_feed_chunk(g_stage, len);
}

static void tftp_close_cb(void *handle)
{
    LWIP_UNUSED_ARG(handle);
    /* lwIP TFTP 在传输结束时调用，交给共享收尾逻辑 */
    ota_on_complete();
}

/* 自愈：擦除 ota_meta 并写入干净的 NONE 状态。
 * 用于清除因历史/异常残留导致的楔形 meta（非法 magic 或非法 state），
 * 避免其掩盖真正 PENDING 或阻断 OTA 握手。仅在 FAL 就绪后调用。 */
static void ota_meta_sanitize(void)
{
    const struct fal_partition *mp = fal_partition_find("ota_meta");
    if (mp == NULL) {
        LOG_OTA("ota: sanitize: meta partition not found\r\n");
        return;
    }
    ota_meta_t clean;
    memset(&clean, 0, sizeof(clean));
    clean.magic = OTA_META_MAGIC;
    clean.state = OTA_STATE_NONE;

    if (flash_kv_mutex != NULL) {
        xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
    }
    int er = fal_partition_erase(mp, 0, OTA_META_SIZE);
    int wr = fal_partition_write(mp, 0, (const uint8_t *)&clean, sizeof(clean));
    if (flash_kv_mutex != NULL) {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }
    if (er != (int)OTA_META_SIZE || wr != (int)sizeof(clean)) {
        LOG_OTA("ota: sanitize FAIL erase=%d write=%d\r\n", er, wr);
    } else {
        LOG_OTA("ota: meta sanitized -> NONE\r\n");
    }
}

/* ---------------- 对外接口 ---------------- */

int ota_handle_upgrade_start(uint32_t new_len, uint32_t new_crc32)
{
    if (g_ota_state != OTA_RX_IDLE)
    {
        LOG_OTA("ota: busy, ignore upgrade_start\r\n");
        return -1;  /* 正在升级中，拒绝新的升级请求 */
    }
    if (new_len == 0 || new_len > OTA_FW_PART_SIZE)
    {
        LOG_OTA("ota: invalid new_len=%lu\r\n", (unsigned long)new_len);
        return -2;  /* 长度非法（0 或超过分区容量 1MB） */
    }
    if (new_crc32 == 0)
    {
        LOG_OTA("ota: invalid new_crc32\r\n");
        return -3;  /* CRC 非法 */
    }

    g_fw_part   = fal_partition_find("firmware_a");
    g_meta_part = fal_partition_find("ota_meta");
    if (g_fw_part == NULL || g_meta_part == NULL)
    {
        LOG_OTA("ota: partition not found\r\n");
        return -4;
    }

    /* 擦除 firmware_a 全部分区（1MB），准备接收新固件 */
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
    }
    fal_partition_erase(g_fw_part, 0, OTA_FW_PART_SIZE);
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }

    g_ota_exp_len = new_len;
    g_ota_exp_crc = new_crc32;
    g_ota_off     = 0;
    g_ota_crc     = 0;
    g_ota_state   = OTA_RX_ACTIVE;

    static const struct tftp_context ctx = {
        .open  = tftp_open_cb,
        .close = tftp_close_cb,
        .read  = tftp_read_cb,
        .write = tftp_write_cb,
    };
    err_t e = tftp_init(&ctx);
    if (e != ERR_OK)
    {
        LOG_OTA("ota: tftp_init fail %d\r\n", (int)e);
        g_ota_state = OTA_RX_IDLE;
        return -5;
    }
    LOG_OTA("ota: upgrade_start accepted, expect len=%lu crc=%08lX, TFTP listening\r\n",
            (unsigned long)new_len, (unsigned long)new_crc32);
    return 0;
}

void ota_app_confirm(void)
{
    static uint8_t s_done = 0;
    if (s_done)
    {
        return;  /* 每上电只判定一次 */
    }

    const struct fal_partition *mp = fal_partition_find("ota_meta");
    if (mp == NULL)
    {
        return;  /* 分区缺失：下个周期重试 */
    }

    ota_meta_t meta;
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
    }
    int rd = fal_partition_read(mp, 0, (uint8_t *)&meta, sizeof(meta));
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }
    if (rd != (int)sizeof(meta))
    {
        return;  /* 读取失败：下个周期重试（避免漏确认导致误回滚） */
    }

    /* 成功观察到一次状态，后续不再重复读取 */
    s_done = 1;

    /* [DEBUG] 启动即打印 ota_meta 当前状态，便于确认上次升级结果 */
    LOG_OTA("ota: meta magic=0x%08lX state=%d (0=NONE,1=PENDING,2=DONE,3=CONFIRMED,4=FAILED)\r\n",
            (unsigned long)meta.magic, (int)meta.state);

    /* 自愈：非法 magic 或非法 state(>FAILED) 视为楔形残留，擦 S16 写 NONE，
     * 避免其掩盖真正的 PENDING 或阻断 OTA 握手。 */
    if (meta.magic != OTA_META_MAGIC || meta.state > OTA_STATE_FAILED)
    {
        LOG_OTA("ota: meta invalid (magic=0x%08lX state=%d) -> sanitize NONE\r\n",
                (unsigned long)meta.magic, (int)meta.state);
        ota_meta_sanitize();
        return;
    }

    if (meta.state != OTA_STATE_DONE)
    {
        /* 合法但非 DONE（NONE/CONFIRMED/PENDING/FAILED）：无需确认 */
        return;
    }

    /* 启动成功确认：DONE → CONFIRMED（防砖确认点） */
    meta.state = OTA_STATE_CONFIRMED;
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
    }
    fal_partition_erase(mp, 0, OTA_META_SIZE);
    fal_partition_write(mp, 0, (const uint8_t *)&meta, sizeof(meta));
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }
    LOG_OTA("ota: new firmware booted OK -> CONFIRMED\r\n");
}
