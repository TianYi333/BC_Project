/*
 * @file           : ota.c
 * @brief          : App 端 OTA 接收 + 启动确认（防砖链路第二部分）
 *
 * 与 boot(main_boot.c) 配合完成 A/B 防砖升级：
 *   1) 网关下发 upgrade{crc,file_size,filename} → 本模块擦 firmware_a、启动
 *      lwIP TFTP server（UDP/69）接收固件（文件名须与命令 filename 一致）；
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
static char           g_ota_exp_fname[OTA_FILENAME_MAX_LEN + 1] = {0}; /* 期望 TFTP 文件名（来自 upgrade 命令） */
static char           g_ota_old_fw_ver[16] = {0}; /* 升级前固件版本，供完成后上报 upgrade_result */

/* TFTP 接收看门狗：开启 TFTP 后超过此时间无任何有效 TFTP 活动（成功 WRQ / 收到 DATA 块），
 * 则中止会话并复位到 IDLE（关闭 TFTP 监听），避免“一直没收到 / 上位机文件名错误不重试 /
 * 传输中途卡死”导致永久卡在 OTA_RX_ACTIVE 而拒绝新的 upgrade 命令。 */
#ifndef OTA_TFTP_RX_TIMEOUT_MS
#define OTA_TFTP_RX_TIMEOUT_MS  30000u   /* 默认 30 秒 */
#endif

static uint32_t      g_ota_last_active_tick = 0;  /* 最近一次有效 TFTP 活动(成功 WRQ/收到 DATA)的 tick(ms，HAL_GetTick) */
static uint32_t      g_ota_rx_timeout_ticks = 0;  /* 由 ms 换算得到的 tick 阈值，monitor 启动时计算 */
static osThreadId    g_ota_mon_task = NULL;

static const struct fal_partition *g_fw_part   = NULL;
static const struct fal_partition *g_meta_part = NULL;

/* 暂存缓冲：容纳 TFTP 单块(512) */
static uint8_t g_stage[1536];

/* ---------------- 内部函数前向声明 ---------------- */
static int  ota_feed_chunk(const uint8_t *data, uint16_t len);
static void ota_reset_session(void);
static void ota_on_complete(void);
static void ota_check_timeout(void);
static void ota_monitor_task(void const *arg);
static void ota_start_monitor(void);
osThreadDef(ota_mon, ota_monitor_task, osPriorityLow, 0, 2048);

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

/* TFTP 接收看门狗：检查自上次有效 TFTP 活动起是否超时，超时则上报并复位会话。
 * 运行于独立低优先级任务，仅读全局状态 + 调用线程安全的上报/复位接口，不碰 lwIP/Flash 锁。 */
static void ota_check_timeout(void)
{
    if (g_ota_state != OTA_RX_ACTIVE)
    {
        return;
    }
    uint32_t now = HAL_GetTick();
    uint32_t elapsed = (uint32_t)(now - g_ota_last_active_tick);  /* 无符号差，自动处理 tick 回绕 */
    if (elapsed >= g_ota_rx_timeout_ticks)
    {
        LOG_OTA("ota: RX timeout %lums, abort & reset session\r\n",
                (unsigned long)elapsed);
        tcp_report_upgrade_status("rx_timeout", "no TFTP traffic, session aborted");
        ota_reset_session();  /* → IDLE + tftp_cleanup()，允许网关重新下发 upgrade */
    }
}

/* 看门狗轮询任务：每秒检查一次，不阻塞 lwIP 收包线程 */
static void ota_monitor_task(void const *arg)
{
    (void)arg;
    for (;;)
    {
        osDelay(1000);
        ota_check_timeout();
    }
}

/* 启动看门狗任务（幂等）。需在 lwIP 已就绪后调用。 */
static void ota_start_monitor(void)
{
    if (g_ota_mon_task != NULL)
    {
        return;  /* 已存在，避免重复创建 */
    }
    if (g_ota_rx_timeout_ticks == 0)
    {
        g_ota_rx_timeout_ticks = OTA_TFTP_RX_TIMEOUT_MS;  /* HAL_GetTick 单位为 ms */
    }
    g_ota_mon_task = osThreadCreate(osThread(ota_mon), NULL);
    if (g_ota_mon_task == NULL)
    {
        LOG_OTA("ota: monitor task create failed\r\n");
    }
}

/* 传输结束（收满或连接关闭）时调用：校验并落盘/复位，或回 IDLE 允许重试 */
static void ota_on_complete(void)
{
    if (g_ota_state != OTA_RX_ACTIVE)
    {
        return;
    }

    /* 文件已收满：先上报 recv_done（进入校验阶段） */
    tcp_report_upgrade_status("recv_done", "firmware received, verifying");

    int complete = (g_ota_off == g_ota_exp_len) && (g_ota_crc == g_ota_exp_crc);

    if (!complete)
    {
        LOG_OTA("ota: abort off=%lu exp=%lu crc=%08lX exp=%08lX\r\n",
                (unsigned long)g_ota_off, (unsigned long)g_ota_exp_len,
                (unsigned long)g_ota_crc, (unsigned long)g_ota_exp_crc);
        tcp_report_upgrade_status("verify_fail", "crc/length mismatch");
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
    /* 判错日志：meta 落盘失败若被静默吞掉，S16 可能停在脏状态，排查无据可依。
     * ⚠️ fal_partition_erase() 成功返回 0（驱动 erase() 返回 0，不是字节数），
     *    只有 <0 才是失败；旧代码写 `er != OTA_META_SIZE` 导致每次成功都误报
     *    "meta erase fail ret=0"，把真正的失败线索掩盖掉了。 */
    if (er < 0) {
        LOG_OTA("ota: meta erase fail ret=%d\r\n", er);
    }
    if (wr != (int)sizeof(meta)) {
        LOG_OTA("ota: meta write fail ret=%d\r\n", wr);
    }

    g_ota_state = OTA_RX_DONE;

    /* 校验通过，即将复位：上报 reboot */
    tcp_report_upgrade_status("reboot", "verify ok, reboot to apply");

    /* 立即复位，boot 将读取 PENDING 并烧录内部 Flash */
    HAL_NVIC_SystemReset();
    /* 不会返回 */
}

/* ---------------- TFTP server 回调 ---------------- */

static void *tftp_open_cb(const char *fname, const char *mode, u8_t write)
{
    LWIP_UNUSED_ARG(mode);

    /* 仅接受写请求（网关 tftp put），且限定文件名须等于本次 upgrade 命令下发的 filename */
    if (write == 0)
    {
        LOG_OTA("ota_tftp: read not supported\r\n");
        return NULL;
    }
    if (strcmp(fname, g_ota_exp_fname) != 0)
    {
        LOG_OTA("ota_tftp: reject file %s (expect %s)\r\n", fname, g_ota_exp_fname);
        /* 文件名门禁失败：显式上报网关，使其可重发正确文件或告警。
         * 注意：本回调运行在 lwIP 收包上下文，禁止在此调用 tftp_cleanup()/ota_reset_session()
         * （会释放当前正在使用的 upcb，随后 lwIP recv 仍要发 ERROR 包 → 崩溃）。
         * TFTP 服务保持监听，网关可用正确文件名重推固件。 */
        tcp_report_upgrade_status("reject_fname", "tftp filename mismatch");
        return NULL;
    }
    if (g_ota_state != OTA_RX_ACTIVE)
    {
        LOG_OTA("ota_tftp: no active upgrade session\r\n");
        return NULL;
    }
    /* 返回非空句柄即可（本模块用全局状态跟踪） */
    g_ota_last_active_tick = HAL_GetTick();  /* 成功 WRQ：刷新有效活动时间，喂看门狗 */
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
    g_ota_last_active_tick = HAL_GetTick();  /* 收到 DATA 块：刷新有效活动时间，喂看门狗 */
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
    /* erase 成功返回 0（<0 才失败）；write 成功返回写入字节数 */
    if (er < 0 || wr != (int)sizeof(clean)) {
        LOG_OTA("ota: sanitize FAIL erase=%d write=%d\r\n", er, wr);
    } else {
        LOG_OTA("ota: meta sanitized -> NONE\r\n");
    }
}

/* ---------------- 对外接口 ---------------- */

int ota_handle_upgrade_start(uint32_t new_len, uint32_t new_crc32,
                             const char *filename)
{
    if (g_ota_state != OTA_RX_IDLE)
    {
        LOG_OTA("ota: busy, ignore upgrade\r\n");
        return -1;  /* 正在升级中，拒绝新的升级请求 */
    }
    if (new_len == 0 || new_len > OTA_FW_PART_SIZE)
    {
        LOG_OTA("ota: invalid new_len=%lu\r\n", (unsigned long)new_len);
        return -2;  /* 长度非法（0 或超过分区容量 384KB） */
    }
    if (new_crc32 == 0)
    {
        LOG_OTA("ota: invalid new_crc32\r\n");
        return -3;  /* CRC 非法 */
    }
    /* 文件名合法性：非空、长度 1..OTA_FILENAME_MAX_LEN(20) */
    if (filename == NULL || filename[0] == '\0' ||
        strlen(filename) > OTA_FILENAME_MAX_LEN)
    {
        LOG_OTA("ota: invalid filename (len must be 1..%u)\r\n",
                (unsigned)OTA_FILENAME_MAX_LEN);
        return -6;  /* 文件名非法 */
    }

    g_fw_part   = fal_partition_find("firmware_a");//查找 firmware_a 分区
    g_meta_part = fal_partition_find("ota_meta");//查找 ota_meta 分区
    if (g_fw_part == NULL || g_meta_part == NULL)
    {
        LOG_OTA("ota: partition not found\r\n");
        return -4;
    }

    /* 擦除 firmware_a 全部分区（384KB），准备接收新固件 */
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreTakeRecursive(flash_kv_mutex, osWaitForever);
    }
    fal_partition_erase(g_fw_part, 0, OTA_FW_PART_SIZE);//擦除 firmware_a 全部分区（384KB）
    if (flash_kv_mutex != NULL)
    {
        xSemaphoreGiveRecursive(flash_kv_mutex);
    }

    /* 记录期望文件名与“升级前版本”（供升级完成后上报 upgrade_result） */
    strncpy(g_ota_exp_fname, filename, OTA_FILENAME_MAX_LEN);
    g_ota_exp_fname[OTA_FILENAME_MAX_LEN] = '\0';
    strncpy(g_ota_old_fw_ver, DEVICE_FW_VER, sizeof(g_ota_old_fw_ver) - 1);
    g_ota_old_fw_ver[sizeof(g_ota_old_fw_ver) - 1] = '\0';
    tcp_store_ota_old_version(g_ota_old_fw_ver);//存储升级前版本

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
    LOG_OTA("ota: upgrade accepted, expect len=%lu crc=%08lX file=%s, TFTP listening\r\n",
            (unsigned long)new_len, (unsigned long)new_crc32, g_ota_exp_fname);

    g_ota_last_active_tick = HAL_GetTick();  /* 开启 TFTP 即算有效活动，避免“一直没收到”立即超时 */
    ota_start_monitor();  /* 确保看门狗任务存在（幂等），负责超时检测 */
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

    /* 升级成功确认：上报 upgrade_result（TCP 未连上则由 net_comm_task 缓存、连上补发） */
    tcp_report_upgrade_result(1, "upgrade_success");

    LOG_OTA("ota: new firmware booted OK -> CONFIRMED\r\n");
}
