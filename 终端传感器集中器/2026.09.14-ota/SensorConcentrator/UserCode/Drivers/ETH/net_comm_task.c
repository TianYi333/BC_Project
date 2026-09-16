#include "net_comm_task.h"
#include "syncif.h"
#include "ota.h"
#include "sensor_hub.h"

//==== 全局静态缓冲区，替代函数内局部大数组 ====
static char sign_buf_req[512];          // 专门给 request 验签用
static char sign_buf_resp[512];         // 专门给 response 组包用
static char sign_buf_online[512];       // 专门给 online 组包用
static char sign_buf_register[512];     // 专门给 register 组包用
// static char tcp_sign_buf_1024[1024];
static char tcp_sign_buf_2048[2048];

// 终端传感器工作模式缓存（下标1-8对应sensor_id），上电从Flash恢复
static uint8_t g_sensor_mode[9] = {0};
// 最近一次各传感器上报ACK状态（下标1-8）：0-成功/未收到，1-失败，2-重试
static uint8_t g_sensor_ack_status[9] = {0};

static char tcp_rx_buf[RX_BUF_SIZE];
static struct tcp_pcb *tcp_pcb = NULL;
static struct udp_pcb *g_udp_discovery_pcb = NULL;
static uint32_t g_tcp_seq = 1;                      //tcp全局自增序列号，上电从 1 开始
static uint32_t g_udp_seq = 1;                      //udp全局自增序列号，上电从 1 开始
volatile static uint32_t g_last_cfg_seq = 0;        //上位机 -> 设备：UDP报文 seq
volatile static uint32_t g_last_cmd_seq = 0;        //上位机 -> 设备：TCP报文 seq 
volatile static uint8_t  g_network_configured = 0;  //配网成功标志：0=未配网 1=已配网
uint8_t g_is_reboot = 1;                            //设备上电状态标记：true=重启/非首次上电  false=首次上电(无历史配网)
static uint32_t reconnect_tick = 0;                 //TCP重连节流计时，避免频繁重连
static volatile uint8_t g_fast_reconnect = 0;       //配网IP变更后跳过重连节流，使用后清零
volatile static uint8_t  g_tcp_connected = 0;       //0=未真正连接 1=握手成功
static uint8_t sntp_inited = 0;                     //SNTP初始化标志
uint64_t sys_unix_ms= 0;                            //系统毫秒对时
static ip4_addr_t  g_server_ip;                     //上位机业务服务器IP（动态保存）
NetConfig_t g_net_cfg;                              //网络配置结构体（固化到内部Flash）
uint16_t g_task_request_cnt = 0;                    //记录当前有效元素个数
uint16_t g_task_order_cnt = 0;                      //记录当前有效元素个数
uint16_t g_task_execute_result_cnt = 0;             //记录当前有效元素个数
char g_stop_target_task_id[48] = {0};               //待停止的任务ID
static volatile uint8_t tcp_pending_close = 0;  // 异步关闭标记

uint8_t g_current_running_inj_id = 0;               // 全局记录当前正在执行注油的出油口ID，0=无任务运行
char g_device_sn[DEV_SN_STR_LEN] = {0};             //全局SN
char g_device_id[DEV_SN_STR_LEN] = {0};             //全局ID
uint8_t g_need_report_reboot_result = 0;
uint8_t g_reboot_result_reported = 0;               // 上电全局一次标记
// 温湿度全局缓存，上报统一放大10倍整型
uint16_t g_sht40_temp_cache = 450;
uint16_t g_sht40_hum_cache  = 380;

void uint64_to_str_2(uint64_t num, char *str, size_t size);
static void udp_recv_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static void udp_send_device_online(struct udp_pcb *upcb, uint16_t reason);
static void udp_msg_process(const char *buf, const ip_addr_t *src_ip, u16_t src_port);
static void tcp_error_callback(void *arg, err_t err);
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf);
static void tcp_send_register(void);
void tcp_send_device_reboot_ack(uint8_t status, const char *message, uint32_t estimated_duration);
void tcp_send_upgrade_start_ack(uint8_t status, const char *message, uint32_t estimated_duration);
void tcp_send_device_reboot_result(uint8_t result, const char *message, const char *reason, uint32_t uptime);
void Lwip_SNTP_Init(const ip_addr_t *ntp_server_ip);
void TimeSyncTask(const void * argument);
void vRebootTask(const void * argument);
void tcp_sender_task(const void * argument);
int8_t init_reboot_db(void);
static void reboot_save_persistent_info(const char *reason);
uint8_t reboot_load_persistent_info(char *reason_buf, size_t buf_len);
static void reboot_clear_persistent_info(void);
void reboot_result_check_and_report(void);
static int8_t sensorhub_write_mode(uint8_t sensor_id, uint8_t mode);
static void sensor_mode_save_flash(void);
static void sensor_mode_load_flash(void);
static void tcp_send_mode_set_resp(uint8_t sensor_id, uint8_t status, uint8_t mode, uint8_t persisted);

// STM32H7 UID基地址
//#define UID_BASE 0x1FF1E800

void GenerateDeviceSNFromUID(void)
{
    uint8_t uid[12];
    // 读取完整12字节芯片唯一ID
    for(uint8_t i = 0; i < 12; i++)
    {
        uid[i] = *((volatile uint8_t *)(UID_BASE + i));
    }

    snprintf(g_device_sn, sizeof(g_device_sn),
             "%02X%02X%02X%02X%02X%02X",
             uid[5], uid[4], uid[3], uid[2], uid[1], uid[0]);
    // ID与SN共用同一串
    strcpy(g_device_id, g_device_sn);

    LOG_NET("Chip full UID: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        uid[0],uid[1],uid[2],uid[3],uid[4],uid[5],uid[6],uid[7],uid[8],uid[9],uid[10],uid[11]);
    LOG_NET("Auto generate DEVICE_SN: %s", g_device_sn);
    LOG_NET("Auto generate DEVICE_ID: %s", g_device_id);
}


void Lwip_SNTP_Init(const ip_addr_t *ntp_server_ip)
{
    sntp_setserver(0, ntp_server_ip); //标准入参：序号+ip_addr_t指针
    // 2.开启SNTP
    sntp_init();
}


int _write(int file, char *ptr, int len)
{
    if (uart_mutex == NULL) return len; // 锁未初始化时直接发送

    xSemaphoreTakeRecursive(uart_mutex, osWaitForever);
    // 临时屏蔽SysTick滴答中断，彻底杜绝抢占UART阻塞收发
    taskENTER_CRITICAL();
    uint16_t idx = 0;
    const uint16_t chunk_size = 128; // 每次最多发128字节，远小于UART FIFO压力
    while (idx < len)
    {
        uint16_t send_len = (len - idx) > chunk_size ? chunk_size : (len - idx);
        HAL_UART_Transmit(&huart2, (uint8_t *)(ptr + idx), send_len, HAL_MAX_DELAY);
        idx += send_len;
    }
    // 恢复SysTick滴答中断
    taskEXIT_CRITICAL();
    xSemaphoreGiveRecursive(uart_mutex);
    return len;
}


// 打印本地IP地址（调试用）
void print_local_ip(void)
{
    char ip_buf[16];
    ipaddr_ntoa_r(&gnetif.ip_addr, ip_buf, sizeof(ip_buf));
    LOG_NET("Local IP: %s", ip_buf);
}


// 网络数据库初始化（单库模式：复用 syncif.c 的共享 kvdb）
int8_t init_net_db(void)
{
    /* kvdb 已由 init_sys_db() 完成初始化（带线程锁）。
     * 此处统一走同一入口，避免多库在内部Flash同一分区上各自GC导致损坏。 */
    int8_t ret = init_sys_db();
    if (ret < 0) {
        LOG_NET("ETH database initialization failed, err=%d", ret);
        return -1;
    }
    LOG_NET("ETH database initialization successful\n");

    // 上电恢复终端传感器工作模式配置（写入 g_sensor_mode[]，供 mode_set 与上报使用）
    sensor_mode_load_flash();
    return 0;
}


//重启数据库初始化函数（单库模式：复用 syncif.c 的共享 kvdb）
int8_t init_reboot_db(void)
{
    int8_t ret = init_sys_db();
    if (ret < 0) {
        LOG_NET("Reboot database initialization failed, err=%d\r\n", ret);
        return -1;
    }
    LOG_NET("Reboot database initialization successful\n");
    return 0;
}


/**
 * @brief 保存重启标记与原因到FlashDB（同net_config_save存储逻辑）
 * @param reason 重启原因字符串
 */
static void reboot_save_persistent_info(const char *reason)
{
    RebootPersistentInfo_t info = {0};
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;

    info.magic = REBOOT_MAGIC_NUM;
    strncpy(info.reason, reason, sizeof(info.reason) - 1);
    info.reason[sizeof(info.reason) - 1] = '\0';

    blob->buf = (uint8_t *)&info;
    blob->size = sizeof(RebootPersistentInfo_t);
    taskENTER_CRITICAL();
    fdb_err_t err = fdb_kv_set_blob(&kvdb, KV_KEY_REBOOT_INFO, blob);
    taskEXIT_CRITICAL();
    if (err != FDB_NO_ERR) {
        LOG_NET("Failed to save reboot info, err=%d. Abort reboot.\n", err);
        return;
    }

    // 写入完成主动解锁
    LOG_NET("Reboot persistent info saved, reason:%s\n", reason);
}


/**
 * @brief 上电读取校验重启标记
 * @param reason_buf 输出原因缓冲区
 * @param buf_len 缓冲区长度
 * @retval 1=存在主动重启标记 0=无有效标记
 */
uint8_t reboot_load_persistent_info(char *reason_buf, size_t buf_len)
{
    RebootPersistentInfo_t info = {0};
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;
    blob->buf = (uint8_t *)&info;
    blob->size = sizeof(RebootPersistentInfo_t);

    // 返回值是读到的字节长度，不是错误码
    size_t read_len = fdb_kv_get_blob(&kvdb, KV_KEY_REBOOT_INFO, blob);

    // 情况1：读取长度为0 → 不存在key / CRC/长度校验损坏
    if(read_len == 0)
    {
        LOG_NET("reboot_info KV empty or length/crc error, delete damaged kv\n");
        fdb_kv_del(&kvdb, KV_KEY_REBOOT_INFO);
        return 0;
    }

    // 情况2：读出长度正常，但magic校验不匹配
    if(info.magic != REBOOT_MAGIC_NUM)
    {
        LOG_NET("reboot_info magic invalid, delete kv\n");
        fdb_kv_del(&kvdb, KV_KEY_REBOOT_INFO);
        return 0;
    }

    // 数据正常拷贝
    strncpy(reason_buf, info.reason, buf_len - 1);
    reason_buf[buf_len - 1] = '\0';
    return 1;
}


/**
 * @brief 清除Flash内重启标记，防止上电重复上报
 */
static void reboot_clear_persistent_info(void)
{
    fdb_kv_del(&kvdb, KV_KEY_REBOOT_INFO);
    LOG_NET("Reboot persistent info cleared\n");
}


//======================== 终端传感器模式配置持久化 =============================
// KV 存储键：终端传感器模式配置（9字节：下标0保留，1-8对应sensor_id）
#define SENSOR_MODE_KV_KEY   "sensor_mode_cfg"

/**
 * @brief 通过Modbus向终端传感器保持寄存器写入工作模式
 * @note  集成点（桩函数）：当前直接返回成功，便于协议闭环联调。
 *        接入真实 Modbus-RTU/ASCII 写保持寄存器后，按从机响应返回：
 *          0 = 成功,  2 = 写入失败,  3 = 传感器离线
 *        真实 Modbus 为阻塞/耗时操作，建议在专属任务中执行；
 *        此处仅更新RAM状态与协议闭环，不阻塞调用方。
 * @retval 0-成功, 2-写入失败, 3-传感器离线
 */
static int8_t sensorhub_write_mode(uint8_t sensor_id, uint8_t mode)
{
    if(sensor_id < 1 || sensor_id > 8) return 2; // 非法ID
    if(mode != SENSOR_MODE_REG && mode != SENSOR_MODE_ACTIVE) return MODE_SET_INVALID;

    // ===== Modbus 写保持寄存器（功能码 06，预留）=====
    // 当前传感器无独立工作模式寄存器（用户确认），默认仅本地记录，不真正下发。
    // 待确定模式寄存器地址后，将 SH_MODE_WRITE_ENABLE 置 1 并填入 SH_REG_MODE 即可。
#if SH_MODE_WRITE_ENABLE
    int8_t ret = SensorHub_WriteReg(sensor_id, SH_REG_MODE, mode);
    if (ret != 0) return MODE_SET_WRITE_FAIL;
#endif
    (void)mode;
    return 0; // 成功（本地记录，持久化由调用方完成）
}

// 读取某传感器本地缓存的工作模式
uint8_t sensorhub_get_mode(uint8_t sensor_id)
{
    if(sensor_id < 1 || sensor_id > 8) return SENSOR_MODE_REG;
    return g_sensor_mode[sensor_id];
}

// 持久化传感器模式配置到共享 kvdb（内部Flash）
static void sensor_mode_save_flash(void)
{
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;
    blob->buf  = g_sensor_mode;
    blob->size = sizeof(g_sensor_mode);
    fdb_kv_set_blob(&kvdb, SENSOR_MODE_KV_KEY, blob);
    LOG_NET("Sensor mode config saved to Flash\n");
}

// 上电从共享 kvdb（内部Flash）恢复传感器模式配置
static void sensor_mode_load_flash(void)
{
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;
    blob->buf  = g_sensor_mode;
    blob->size = sizeof(g_sensor_mode);
    size_t len = fdb_kv_get_blob(&kvdb, SENSOR_MODE_KV_KEY, blob);
    if(len == sizeof(g_sensor_mode))
    {
        LOG_NET("Sensor mode config loaded from Flash\n");
    }
    else
    {
        // 无有效记录，重置为默认（寄存器模式）
        memset(g_sensor_mode, 0, sizeof(g_sensor_mode));
    }
}


//上电检查重启标记并上报结果（TCP 连接成功后调用）
void reboot_result_check_and_report(void)
{
    char reason_buf[64] = {0};
    uint8_t is_active_reboot = reboot_load_persistent_info(reason_buf, sizeof(reason_buf));

    if(is_active_reboot)
    {
        uint32_t uptime_sec = HAL_GetTick() / 1000U;
        tcp_send_device_reboot_result(0, "reboot_success", reason_buf, uptime_sec);
        LOG_NET("Report reboot success, reason:%s, uptime:%lus\n", reason_buf, uptime_sec);
        // 上报完成清除标记，避免重复上报
        reboot_clear_persistent_info();
    }
}


// 上电读取Flash保存的网络配置，自动配置网卡IP
void net_config_init(void)
{
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;

    /* 本函数在 StartDefaultTask 中先于 Task_InitTask 调用，
     * 而 kvdb 的初始化（init_sys_db）原本放在 Task_InitTask 里。
     * 必须先确保 kvdb 已初始化，否则下面 fdb_kv_get_blob 会访问未初始化的
     * 全零 kvdb（NULL 解引用）→ HardFault。init_sys_db 内部有 g_kvdb_inited
     * 幂等保护，重复调用安全。 */
    //init_sys_db();

    memset(&g_net_cfg, 0, sizeof(NetConfig_t));

    blob->buf = (uint8_t *)&g_net_cfg;
    blob->size = sizeof(NetConfig_t);

    size_t len = fdb_kv_get_blob(&kvdb, "net_cfg", blob);

    if(len == sizeof(NetConfig_t) && g_net_cfg.configured == 1)
    {
        ip4_addr_t ip, mask, gw, srv_ip;
        IP4_ADDR(&ip,       g_net_cfg.ip[0],        g_net_cfg.ip[1],
                            g_net_cfg.ip[2],        g_net_cfg.ip[3]);
        IP4_ADDR(&mask,     g_net_cfg.netmask[0],   g_net_cfg.netmask[1],
                            g_net_cfg.netmask[2],   g_net_cfg.netmask[3]);
        IP4_ADDR(&gw,       g_net_cfg.gateway[0],   g_net_cfg.gateway[1],
                            g_net_cfg.gateway[2],   g_net_cfg.gateway[3]);
        IP4_ADDR(&srv_ip,   g_net_cfg.server_ip[0], g_net_cfg.server_ip[1],
                            g_net_cfg.server_ip[2], g_net_cfg.server_ip[3]);

        netifapi_netif_set_addr(&gnetif, &ip, &mask, &gw);
        netifapi_autoip_stop(&gnetif);
        netifapi_netif_set_down(&gnetif);
        netifapi_netif_set_up(&gnetif);

        gnetif.flags |= NETIF_FLAG_BROADCAST;

        g_server_ip = srv_ip;
        g_network_configured = 1;
        LOG_NET("Flash loading network configuration succeeded, TCP started\n");
        if(sntp_inited == 0)
        {
            Lwip_SNTP_Init(&g_server_ip);
            sntp_inited = 1;
        }
    }
    else
    {
        netifapi_autoip_start(&gnetif);
        g_network_configured = 0;
        LOG_NET("Not connected to the network, starting AutoIP\n");
        ip_addr_t default_ntp_ip;
        IP4_ADDR(&default_ntp_ip,192,168,0,10);
        if(sntp_inited == 0)
        {
            Lwip_SNTP_Init(&default_ntp_ip);
            sntp_inited = 1;
            LOG_NET("AutoIP OK, start SNTP\n");
        }
    }
}


// 保存配网参数 to FlashDB
void net_config_save(void)
{
    g_net_cfg.configured = 1;

    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;
    blob->buf = (uint8_t *)&g_net_cfg;
    blob->size = sizeof(NetConfig_t);

    fdb_kv_set_blob(&kvdb, "net_cfg", blob);

    LOG_NET("The distribution network parameters have been saved to Flash\n");
}


extern u8_t sntp_enabled(void);
void NTP_Force_Refresh(void)//快速手动刷新
{
    if(g_network_configured == 0)
    {
        LOG_NET("NTP skip: network not ready\n");
        return;
    }
    sys_unix_ms = 0U;
    if(sntp_enabled() != 0U)
    {
        sntp_stop();
    }
    sntp_init();
}


//先写时间戳转日期函数，无依赖time.h也能用
static void stamp_to_time(uint64_t ms_stamp, char *buf, uint16_t buf_len)
{
    if(buf == NULL || buf_len < 22)
    {
        if(buf) buf[0] = '\0';
        return;
    }
    uint64_t sec = ms_stamp / 1000;
    uint32_t ms  = (uint32_t)(ms_stamp % 1000);
    time_t t = (time_t)sec;
    struct tm *tm_t = localtime(&t);
    if(tm_t == NULL)
    {
        buf[0] = '\0';
        return;
    }

    snprintf(buf, buf_len, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
        tm_t->tm_year + 1900,
        tm_t->tm_mon + 1,
        tm_t->tm_mday,
        tm_t->tm_hour,
        tm_t->tm_min,
        tm_t->tm_sec,
        (int)ms);
}


// 安全销毁UDP PCB，释放资源防内存泄漏
static void udp_discovery_pcb_destroy(void)
{
    if(udp_pcb_mutex == NULL) return;
    xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
    if(g_udp_discovery_pcb != NULL)
    {
        udp_recv(g_udp_discovery_pcb, NULL, NULL); // 清空中断回调，避免野指针
        udp_remove(g_udp_discovery_pcb);
        g_udp_discovery_pcb = NULL;
        LOG_NET("UDP old PCB destroyed\n");
    }
    xSemaphoreGiveRecursive(udp_pcb_mutex);
}


// 创建并绑定UDP PCB，返回0成功/-1失败
static int8_t udp_discovery_pcb_create(void)
{
    udp_discovery_pcb_destroy(); // 先销毁旧实例
    xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);

    gnetif.flags |= NETIF_FLAG_BROADCAST;

    struct udp_pcb *new_pcb = udp_new();
    if(new_pcb == NULL)
    {
        LOG_NET("udp_new() failed, pcb pool exhausted\n");
        xSemaphoreGiveRecursive(udp_pcb_mutex);
        return -1;
    }

    err_t bind_ret = udp_bind(new_pcb, IP_ADDR_ANY, UDP_LISTEN_PORT);
    if(bind_ret != ERR_OK)
    {
        LOG_NET("udp_bind fail, err:%d", bind_ret);
        udp_remove(new_pcb);
        xSemaphoreGiveRecursive(udp_pcb_mutex);
        return -1;
    }

    new_pcb->so_options |= SOF_BROADCAST;
    gnetif.flags |= NETIF_FLAG_BROADCAST;// 开启网口广播权限
    udp_recv(new_pcb, udp_recv_callback, NULL);

    g_udp_discovery_pcb = new_pcb;

    LOG_NET("UDP PCB create success, port:%d", new_pcb->local_port);
    LOG_NET("SOF_BROADCAST:%s netif_flags:0x%08X",
        (new_pcb->so_options & SOF_BROADCAST) ? "YES" : "NO", gnetif.flags);

    xSemaphoreGiveRecursive(udp_pcb_mutex);
    return 0;
}


/**
 * @brief  UDP设备发现任务，端口50000（始终运行）
 * @note   监听上位机UDP广播发现报文，回复设备信息
 *         仅开启广播接收，AutoIP模式无法使用组播
 */
void Udp_discover_task(const void * argument)
{
    UdpMsgTypeDef udp_msg;
    ip4_addr_t local_ip;
    ip4_addr_t last_valid_ip = IPADDR4_INIT(0);

    while(1)
    {
        local_ip = *netif_ip4_addr(&gnetif);
        /* 必须同时满足"IP 有效"与"物理链路已 up"才能启动 UDP 服务：
         * 静态 IP 场景下 net_config_init() 设完 IP 后 ip4_addr_isany() 立刻为假，
         * 但 PHY 自动协商通常还要 1~8 秒才 link up。若只看 IP，设备会在 netif 仍 down 时
         * 发出 device_online 广播 -> udp_sendto 返回 ERR_RTE(-4)，NTP 同步同理失败。
         * （AutoIP 时代无此问题：AutoIP 必须 link up 才协商出 IP，
         *   "IP 有效"隐含了"link 已 up"；改成静态 IP 后这层隐含保护消失，bug 暴露。）
         * 实测现象：上电后 device_online 先报 status:-4，约 8 秒后才 [ETH] link UP。 */
        if(!ip4_addr_isany(&local_ip) && netif_is_link_up(&gnetif))
        {
            last_valid_ip = local_ip;
            break;
        }
        LOG_NET("Waiting for auto link ip...\n");
        osDelay(1000);
    }
    print_local_ip();
    LOG_NET("Valid IP obtained, start UDP service\n");

    // 初始化创建UDP PCB，赋值全局句柄
    if(udp_discovery_pcb_create() == 0)
    {
        osDelay(5000);
        // 上面 5s 延时期间链路可能又断开，发送前再确认一次，避免再次出现 send status:-4(ERR_RTE)
        if(netif_is_link_up(&gnetif))
        {
            xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
            udp_send_device_online(g_udp_discovery_pcb,0);
            xSemaphoreGiveRecursive(udp_pcb_mutex);
        }
        else
        {
            LOG_NET("Link down again, skip device_online\n");
        }
    }
    else
    {
        LOG_NET("udp_new() failed, no pcb available!\n");
    }

    while(1)
    {
        //osDelay(2000);
        // const char *pure_json_buf_2 = "{\"domain\":\"FACTORY_OIL\",\"gateway_mac\":\"1a:f5:79:d4:5e:32\",\"seq\":1,\"ts\":1776996714,\"type\":\"discover_request\"}";
        
        // LOG_NET("Sorted JSON: %s\r\n", pure_json_buf_2);
        // LOG_NET("Sorted JSON Length: %lu\r\n", (unsigned long)strlen(pure_json_buf_2));
        // uint8_t gw_calc_hash[32] = {0};

        // hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        // hmac_sha256_update(&hmac, (const uint8_t *)pure_json_buf_2, strlen(pure_json_buf_2));
        // hmac_sha256_final(&hmac, gw_calc_hash);

        // char gw_calc_sign[9] = {0};
        // snprintf(gw_calc_sign, sizeof(gw_calc_sign), "%02X%02X%02X%02X",
        //         gw_calc_hash[0], gw_calc_hash[1],
        //         gw_calc_hash[2], gw_calc_hash[3]);
        // LOG_NET("Calc sign: %s\r\n", gw_calc_sign);
        // 1. 读取当前网卡IP，对比上一次缓存IP，判断是否变更

        if(xQueueReceive(udp_msg_queue, &udp_msg, pdMS_TO_TICKS(100)) == pdPASS)
        {
            // 防护1：PCB被销毁/重建时为空，直接跳过，防止野指针传入udp_msg_process
            if(g_udp_discovery_pcb == NULL)
            {
                LOG_NET("UDP PCB invalid, skip this udp packet\n");
                continue;
            }
            // 防护2：互斥锁保护PCB，中断回调与任务线程并发操作lwIP资源
            xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
            udp_msg_process(udp_msg.data, &udp_msg.src_ip, udp_msg.src_port);
            xSemaphoreGiveRecursive(udp_pcb_mutex);
        }

        ip4_addr_t curr_ip = *netif_ip4_addr(&gnetif);
        uint8_t ip_changed = !ip4_addr_cmp(&curr_ip, &last_valid_ip);
        uint8_t ip_valid = !ip4_addr_isany(&curr_ip);
        uint8_t need_rebuild = 0;

        if(ip_changed)
        {
            LOG_NET("Local IP changed, mark UDP rebuild\n");
            need_rebuild = 1;
            last_valid_ip = curr_ip;
            print_local_ip();
        }

        // IP无效，销毁UDP等待
        if(!ip_valid)
        {
            udp_discovery_pcb_destroy();
            osDelay(1000);
            continue;
        }

        // IP变更执行重建
        if(need_rebuild)
        {
            udp_discovery_pcb_destroy();
            osDelay(800);
            if(udp_discovery_pcb_create() == 0)
            {
                osDelay(3000);
                if(netif_is_link_up(&gnetif))   // 同上：发送前确认链路仍 up
                {
                    xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
                    udp_send_device_online(g_udp_discovery_pcb,0);
                    xSemaphoreGiveRecursive(udp_pcb_mutex);
                }
                else
                {
                    LOG_NET("Link down again, skip device_online\n");
                }
            }
            else
            {
                LOG_NET("UDP PCB create failed, retry after 2s\n");
                osDelay(2000);
                continue;
            }
        }

        //循环读取 PHY 链路状态
        // int32_t link = LAN8742_GetLinkState(&LAN8742);
        // uint32_t flags = gnetif.flags;
        // LOG_NET("LinkState:%d, NetIfFlags:0x%08X\r\n", link, flags);
        // osDelay(10000);
    }
}


//带大小参数的版本，避免缓冲区溢出风险）
void uint64_to_str_2(uint64_t num, char *str, size_t size) {
    if (size == 0) return;  // 没有空间可写
    if (size == 1) {
        str[0] = '\0';
        return;
    }

    int i = 0;
    if (num == 0) {
        str[i++] = '0';
    } else {
        char temp[21];  // 64位十进制最多20位
        int j = 0;
        while (num > 0) {
            temp[j++] = (num % 10) + '0';
            num /= 10;
        }
        // 倒序复制，但不超过 size-1
        while (j > 0 && i < (int)size - 1) {
            str[i++] = temp[--j];
        }
    }
    str[i] = '\0';
}

void uint64_to_str(uint64_t num, char* str) {
    int i = 0;
    if (num == 0) {
        str[i++] = '0';
    } else {
        char temp[21]; 
        int j = 0;
        while (num > 0) {
            temp[j++] = (num % 10) + '0';
            num /= 10;
        }
        // 倒序复制
        while (j > 0) {
            str[i++] = temp[--j];
        }
    }
    str[i] = '\0'; 
}

// 通过 UDP 发送 device_online 上线报文（广播发送）
static void udp_send_device_online(struct udp_pcb *upcb, uint16_t reason)
{
    // 加锁保护PCB与pbuf分配
    xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
    if(upcb == NULL) 
    {
        xSemaphoreGiveRecursive(udp_pcb_mutex);
        return;
    }
    // 网卡未就绪直接退出，防止IP接口非法访问
    if( !(gnetif.flags & NETIF_FLAG_UP) )
    {
        LOG_NET("netif not up, skip udp online broadcast\r\n");
        xSemaphoreGiveRecursive(udp_pcb_mutex);
        return;
    }
    gnetif.flags |= NETIF_FLAG_BROADCAST;// 确保广播权限

    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Get_Unix_Second();
    char mac_str[32] = {0};
    uint8_t *mac = gnetif.hwaddr;

    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memset(sign_buf_online, 0, sizeof(sign_buf_online));
    char ts_online_str[21] = {0};
    uint64_to_str(ts, ts_online_str);
    
    const char *boot_str;
    int boot_json;
    if(g_network_configured)
    {
        boot_str  = "true";
        boot_json  = cJSON_True;
    }
    else
    {
        boot_str  = "false";
        boot_json  = cJSON_False;
    }
    g_udp_seq=1;//UDP序列号上电重置，避免与历史遗留数据冲突导致误判
    // 签名原文（保持原有字典序不变）
    snprintf(sign_buf_online, sizeof(sign_buf_online)-1,
            "{\"cur_ip\":\"%s\",\"firmware_ver\":\"%s\",\"is_reboot\":%s,\"mac\":\"%s\",\"model\":\"%s\",\"reason\":%d,\"seq\":%lu,\"sn\":\"%s\",\"ts\":%s,\"type\":\"%s\"}",
            ip4addr_ntoa(netif_ip4_addr(&gnetif)),
            DEVICE_FW_VER,
            boot_str,
            mac_str,
            DEVICE_MODEL,
            reason,
            (unsigned long)g_udp_seq,
            g_device_sn,
            ts_online_str,
            "device_online");
    //打印签名校验原文，调试用
    // LOG_NET("UDP Sorted device_online JSON:\r\n%s\r\n", sign_buf_online);
    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac;
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_online, strlen(sign_buf_online));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 组装完整报文
    cJSON_AddStringToObject(root, "type", "device_online");
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "sn", g_device_sn);
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(root, "firmware_ver", DEVICE_FW_VER);
    cJSON_AddStringToObject(root, "cur_ip", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
    cJSON_AddBoolToObject(root, "is_reboot", boot_json);//true=重启，false=首次上电
    cJSON_AddNumberToObject(root, "reason", reason);//0-上电，1-看门狗，2-软件重启（目前统一暂为0）
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "seq", g_udp_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG_NET("UDP Send JSON:\r\n%s\r\n", str);
        uint16_t json_len = strlen(str);
        uint16_t send_len = json_len + 1;  // +1 存换行符
        struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, send_len, PBUF_POOL);
        if(send_len > 1472)
        {
            LOG_NET("udp json too long, drop\r\n");
            cJSON_PortFree(str);
            cJSON_Delete(root);
            xSemaphoreGiveRecursive(udp_pcb_mutex);
            return;
        }
        if(p_tx != NULL)
        {

            memcpy(p_tx->payload, str, json_len);
            ((uint8_t *)p_tx->payload)[json_len] = '\n'; // 追加换行
            // UDP 广播发送（对应上位机发现端口 UDP_LISTEN_PORT）
            err_t ret = udp_sendto(upcb, p_tx, IP_ADDR_BROADCAST, UDP_LISTEN_PORT);
            (void)ret;
            LOG_NET("UDP device_online send status:%d\r\n", ret);
            pbuf_free(p_tx);
        }
        else
        {
            // 内存池不足日志
            LOG_NET("pbuf alloc failed, pool empty\r\n");
        }
        cJSON_PortFree(str);
        g_udp_seq++;
    }
    cJSON_Delete(root);
    xSemaphoreGiveRecursive(udp_pcb_mutex);
}


// UDP报文统一处理分发
static void udp_msg_process(const char *buf, const ip_addr_t *src_ip, u16_t src_port)
{ 
    HMAC_SHA256_CTX hmac;
    LOG_NET("UDP Recv JSON:\r\n%s\r\n", buf);
    cJSON *root = cJSON_Parse(buf);
    if(root == NULL) return;

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if(!type)
    {
        cJSON_Delete(root);
        return;
    }

    // 设备发现请求
    if(strcmp(type->valuestring, "discover_request") == 0)
    {
        cJSON *domain          = cJSON_GetObjectItem(root, "domain");
        cJSON *gateway_mac     = cJSON_GetObjectItem(root, "gateway_mac");
        cJSON *ts              = cJSON_GetObjectItem(root, "ts");
        cJSON *seq             = cJSON_GetObjectItem(root, "seq");
        cJSON *sign            = cJSON_GetObjectItem(root, "sign");

        if(!domain || domain->type != cJSON_String)            goto udp_task_err;
        if(!gateway_mac || gateway_mac->type != cJSON_String)  goto udp_task_err;
        if(!ts || ts->type != cJSON_Number)                    goto udp_task_err;
        if(!seq || seq->type != cJSON_Number)                  goto udp_task_err;
        if(!sign || sign->type != cJSON_String)                goto udp_task_err;

        if(strcmp(domain->valuestring, "FACTORY_OIL") != 0)    goto udp_task_err;

        // 时间戳校验：与当前时间差 <60秒
        uint64_t now_ts = Get_Unix_Second();
        int64_t  time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
        if (time_diff > TIME_VALID_SEC)  
        {
            LOG_NET("Time verification failed!\r\n");
            // goto udp_task_err;
        }
        uint32_t curr_gw_seq = (uint32_t)seq->valuedouble;
         if(curr_gw_seq == g_last_cfg_seq )
         {
            LOG_NET("Sequence number verification failed!\r\n");
            // goto udp_task_err;   
         } 
        g_last_cfg_seq = curr_gw_seq;


        memset(sign_buf_req, 0, sizeof(sign_buf_req));
        char ts_str[21] = {0};
        uint64_t ts_num = (uint64_t)ts->valuedouble;
        uint64_to_str(ts_num, ts_str);

        // 按键名字典序构造无sign原始JSON
        snprintf(sign_buf_req, sizeof(sign_buf_req)-1,
            "{\"domain\":\"%s\",\"gateway_mac\":\"%s\",\"seq\":%lu,\"ts\":%s,\"type\":\"%s\"}",
            domain->valuestring,
            gateway_mac->valuestring,
            (unsigned long)curr_gw_seq,
            ts_str,
            "discover_request");

        //LOG_NET("UDP Sorted discover_request JSON:\r\n%s\r\n", sign_buf_req);

        uint8_t gw_calc_hash[32] = {0};
        hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_req, strlen(sign_buf_req));
        hmac_sha256_final(&hmac, gw_calc_hash);

        char gw_calc_sign[9] = {0};
        snprintf(gw_calc_sign, sizeof(gw_calc_sign), "%02X%02X%02X%02X",
                gw_calc_hash[0], gw_calc_hash[1],
                gw_calc_hash[2], gw_calc_hash[3]);
        //LOG_NET("Calc discover_request sign: %s\r\n", gw_calc_sign);      
        //签名不匹配 → 非法报文
        if (strcmp(sign->valuestring, gw_calc_sign) != 0) {
             LOG_NET("sign check failed!\r\nExpected: %s, Actual: %s\r\n", gw_calc_sign, sign->valuestring);
             goto udp_task_err;
        }

        {
            cJSON *resp = cJSON_CreateObject();
            uint8_t *mac_addr = gnetif.hwaddr;
            char mac_str[32];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                    mac_addr[0], mac_addr[1], mac_addr[2],
                    mac_addr[3], mac_addr[4], mac_addr[5]);
            cJSON_AddStringToObject(resp, "type", "discover_response");
            cJSON_AddStringToObject(resp, "mac", mac_str);
            cJSON_AddStringToObject(resp, "sn", g_device_sn);
            cJSON_AddStringToObject(resp, "model", DEVICE_MODEL);
            cJSON_AddStringToObject(resp, "firmware_ver", DEVICE_FW_VER);
            cJSON_AddStringToObject(resp, "cur_ip", ip4addr_ntoa(netif_ip4_addr(&gnetif)));

            uint64_t real_ts = Get_Unix_Second();
            cJSON_AddNumberToObject(resp, "ts", real_ts);
            //cJSON_AddNumberToObject(resp, "ts", ts_num+20);//调试阶段使用发送来的时间戳
            cJSON_AddNumberToObject(resp, "seq", g_udp_seq);

            memset(sign_buf_resp, 0, sizeof(sign_buf_resp));
            char ts_buf[21] = {0};
            uint64_to_str(real_ts, ts_buf);
            // uint64_to_str(ts_num+20, ts_buf);//调试阶段使用发送来的时间戳

            snprintf(sign_buf_resp, sizeof(sign_buf_resp)-1,
                "{\"cur_ip\":\"%s\",\"firmware_ver\":\"%s\",\"mac\":\"%s\",\"model\":\"%s\",\"seq\":%u,\"sn\":\"%s\",\"ts\":%s,\"type\":\"%s\"}",
                ip4addr_ntoa(netif_ip4_addr(&gnetif)),
                DEVICE_FW_VER,
                mac_str,
                DEVICE_MODEL,
                (unsigned int)g_udp_seq,
                g_device_sn,
                ts_buf,
                "discover_response"
            );
            //LOG_NET("UDP Sorted discover_response JSON:\r\n%s\r\n", sign_buf_resp);

            uint8_t sha256_result[32] = {0};
            hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
            hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_resp, strlen(sign_buf_resp));
            hmac_sha256_final(&hmac, sha256_result);

            char sign_str[9] = {0};
            snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
                    sha256_result[0],
                    sha256_result[1],
                    sha256_result[2],
                    sha256_result[3]);
            LOG_NET("Calc discover_response sign: %s\r\n", sign_str);  

            cJSON_AddStringToObject(resp, "sign", sign_str);

            char *json_reply = cJSON_PrintUnformatted(resp);
            if(json_reply != NULL)
            {
                LOG_NET("UDP Send JSON:\r\n%s\r\n", json_reply);
                uint16_t json_len = strlen(json_reply);
                uint16_t send_len = json_len + 1;// JSON + \n 总长度
                // 按总发送长度分配pbuf
                 xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
                struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, send_len, PBUF_POOL);
                if(p_tx != NULL)
                {
                    memcpy(p_tx->payload, json_reply, json_len);
                    ((uint8_t *)p_tx->payload)[json_len] = '\n';// 补换行
                    err_t ret = udp_sendto(g_udp_discovery_pcb, p_tx, IP_ADDR_BROADCAST, UDP_LISTEN_PORT);
                    (void)ret;
                    LOG_NET("UDP send status:%d\r\n", ret);
                    pbuf_free(p_tx);
                }
                xSemaphoreGiveRecursive(udp_pcb_mutex);
                cJSON_PortFree(json_reply);
                g_udp_seq++;
            }
            cJSON_Delete(resp);
        }
    }
    // UDP接收配网配置报文
    else if(strcmp(type->valuestring, "config_set") == 0)
    {
        cJSON *target_mac  = cJSON_GetObjectItem(root, "target_mac");
        cJSON *target_sn   = cJSON_GetObjectItem(root, "target_sn");
        cJSON *ip          = cJSON_GetObjectItem(root, "ip");
        cJSON *netmask     = cJSON_GetObjectItem(root, "netmask");
        cJSON *gateway     = cJSON_GetObjectItem(root, "gateway");
        cJSON *host_ip     = cJSON_GetObjectItem(root, "host_ip");
        cJSON *host_mac    = cJSON_GetObjectItem(root, "host_mac");
        cJSON *ts          = cJSON_GetObjectItem(root, "ts");
        cJSON *seq         = cJSON_GetObjectItem(root, "seq");
        cJSON *sign        = cJSON_GetObjectItem(root, "sign");

        if(!target_mac || !target_sn || !ip || !netmask || !gateway || !host_ip || !host_mac ||
            !ts || !seq || !sign)
        {
            LOG_NET("KEY value verification failed!\r\n");
            goto udp_task_err;
        }

        char local_mac[32] = {0};
        snprintf(local_mac, sizeof(local_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
                gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);

        if(strcmp(target_mac->valuestring, local_mac) != 0)
        {
            LOG_NET("MAC address verification failed!\r\n");
            goto udp_task_err;
        }

        if(strcmp(target_sn->valuestring, g_device_sn) != 0)
        {
            LOG_NET("SN verification failed!\r\n");
            goto udp_task_err;
        }

        uint64_t now_ts = Get_Unix_Second();
        int64_t time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
         if(time_diff > TIME_VALID_SEC)
        {
            LOG_NET("Time verification failed!\r\n");
            // goto udp_task_err;
        }

        uint32_t curr_seq = (uint32_t)seq->valuedouble;
        if(curr_seq == g_last_cfg_seq)
        {
            LOG_NET("Sequence number verification failed!\r\n");
            // goto udp_task_err;
        }
         g_last_cfg_seq = curr_seq;

        memset(sign_buf_req, 0, sizeof(sign_buf_req));
        char ts_str_config[21] = {0};
        uint64_to_str((uint64_t)ts->valuedouble, ts_str_config);

        snprintf(sign_buf_req, sizeof(sign_buf_req)-1,
            "{\"gateway\":\"%s\",\"host_ip\":\"%s\",\"host_mac\":\"%s\",\"ip\":\"%s\",\"netmask\":\"%s\",\"seq\":%lu,\"target_mac\":\"%s\",\"target_sn\":\"%s\",\"ts\":%s,\"type\":\"%s\"}",
            gateway->valuestring,
            host_ip->valuestring,
            host_mac->valuestring,
            ip->valuestring,
            netmask->valuestring,
            (unsigned long)curr_seq,
            target_mac->valuestring,
            target_sn->valuestring,
            ts_str_config,
            "config_set");


        //LOG_NET("UDP Sorted config_set JSON:\r\n%s\r\n", sign_buf_req);

        uint8_t calc_hash[32] = {0};
        hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_req, strlen(sign_buf_req));
        hmac_sha256_final(&hmac, calc_hash);

        char calc_sign[9] = {0};
        snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X",
                calc_hash[0], calc_hash[1], 
                calc_hash[2], calc_hash[3]);

        //LOG_NET("UDP Calc config_set sign:%s\r\n", calc_sign);

        if(strcmp(sign->valuestring, calc_sign) != 0)
        {
            LOG_NET("sign check failed!\r\nExpected: %s, Actual: %s\r\n", calc_sign, sign->valuestring);
            goto udp_task_err;
        }

        ip4_addr_t ip_addr, mask_addr, gw_addr, srv_addr;
        ipaddr_aton(ip->valuestring, &ip_addr);
        ipaddr_aton(netmask->valuestring, &mask_addr);
        ipaddr_aton(gateway->valuestring, &gw_addr);
        ipaddr_aton(host_ip->valuestring, &srv_addr);

        LOCK_TCPIP_CORE();
        netif_set_ipaddr(&gnetif, &ip_addr);//设置网卡本机 IPv4 地址
        netif_set_netmask(&gnetif, &mask_addr);//设置子网掩码，划定同网段范围
        netif_set_gw(&gnetif, &gw_addr);//设置网关，跨网段转发路由出口


        g_server_ip = srv_addr;
        g_network_configured = 1;

        memcpy(g_net_cfg.ip, &ip_addr.addr, 4);
        memcpy(g_net_cfg.netmask, &mask_addr.addr, 4);
        memcpy(g_net_cfg.gateway, &gw_addr.addr, 4);
        memcpy(g_net_cfg.server_ip, &srv_addr.addr, 4);

        net_config_save();
        autoip_stop(&gnetif);// 关闭自动IP，防止篡改固定地址
        netif_set_down(&gnetif);// 重启网口让新IP正式生效
        netif_set_up(&gnetif);
        UNLOCK_TCPIP_CORE();
        gnetif.flags |= NETIF_FLAG_BROADCAST;// 重启后再次确保广播权限
        // IP/服务端IP变更，立刻断开旧TCP
        if(tcp_pcb != NULL)
        {
            LOG_NET("Server IP changed, mark async close");
            tcp_pending_close = 1;   // 统一异步关闭，由主循环处理
            // 重置重连计时器，让重连更快触发（跳过 reconnect_interval 等待）
            reconnect_tick = HAL_GetTick() - 5000;
            g_fast_reconnect = 1;   // 配网IP变更：标记快速重连，重连门将跳过节流
        }
        LOG_NET("Distribution network successfully and Flash saved\r\n");
        print_local_ip();
        osDelay(300); // 短暂延时等待网络/IP稳定

        if(sntp_inited == 0)
        {
            Lwip_SNTP_Init(&g_server_ip);
            sntp_inited = 1;
        }
        else//已经开过sntp，IP变了重启一次
        {
            sntp_stop();
            Lwip_SNTP_Init(&g_server_ip);
            LOG_NET("SNTP updated to new server ip: %s", ip4addr_ntoa(&g_server_ip));
        }
        // IP变更后销毁旧UDP，重建UDP服务
        if(udp_pcb_mutex != NULL)
        {
            udp_discovery_pcb_destroy();
            osDelay(500);
            if(udp_discovery_pcb_create() == 0)
            {
                osDelay(3000); // 等待网络稳定
                xSemaphoreTakeRecursive(udp_pcb_mutex, portMAX_DELAY);
                udp_send_device_online(g_udp_discovery_pcb,2);
                xSemaphoreGiveRecursive(udp_pcb_mutex);
            }
            else
            {
                LOG_NET("Recreate UDP PCB after config_set failed");
            }
        }
        else
        {
            LOG_NET("udp mutex not init, skip udp rebuild");
        }
    }

udp_task_err:
    cJSON_Delete(root);
}


/**
 * @brief  UDP接收回调函数
 * @param  addr: 上位机IP地址
 * @param  port: 上位机端口
 * @note   回调仅拷贝入队，不做解析运算，规避栈溢出
 */
static void udp_recv_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    if(p == NULL) return;
    if(udp_msg_queue == NULL)
    {
        pbuf_free(p);
        return;
    }

    UdpMsgTypeDef msg;
    u16_t total_len = p->tot_len;
    uint16_t copy_len = total_len < sizeof(msg.data)-1 ? total_len : (sizeof(msg.data)-1);
    pbuf_copy_partial(p, msg.data, copy_len, 0);
    msg.data[copy_len] = '\0'; // 字符串结束符
    msg.len = copy_len;
    const ip4_addr_t *ipv4_addr = (const ip4_addr_t *)addr;
    msg.src_ip = *ipv4_addr;
    msg.src_port = port;

    BaseType_t ret = xQueueSend(udp_msg_queue, &msg, 0);
    if(ret != pdPASS)
    {
        LOG_NET("UDP queue full, drop udp packet, len:%u", copy_len);
    }
    pbuf_free(p);
}


//=====================================================================
/**
 * @brief 安全关闭TCP连接，全局互斥锁保护PCB操作
 */
static void safe_tcp_close(void)
{
    // if ((lock_tcpip_core == NULL) || (tcp_send_mutex == NULL))
    // {
    //     return;     // lwIP 还没初始化完，直接跳过
    // }
    // 先获取lwIP核心锁，再拿tcp_send_mutex；与tcp_sender_task/connected_cb保持一致顺序(core->mutex)，避免死锁
    LOCK_TCPIP_CORE();
    if(xSemaphoreTakeRecursive(tcp_send_mutex, osWaitForever) != pdPASS)
    {
        UNLOCK_TCPIP_CORE();
        return;
    }

    // 统一释放锁标记，所有分支最终跳转到这里释放互斥锁
    do {
        if(tcp_pcb == NULL)
        {
            // PCB已经为空，无需处理，直接跳出
            break;
        }

        // 先尝试优雅关闭，让 lwIP 完成未发送数据/Fin 的发送；
        // 只有 tcp_close 无法完成时才回退到 tcp_abort，尽量减少
        // pending 在 ETH DMA 中的 pbuf 被 pcb purge 二次释放的风险。
        struct tcp_pcb *pcb = tcp_pcb;
        tcp_pcb = NULL;
        g_tcp_connected = 0;

        err_t close_ret = tcp_close(pcb);
        if(close_ret != ERR_OK)
        {
            tcp_abort(pcb);
            LOG_NET("TCP tcp_close fail %d, fallback tcp_abort", (int)close_ret);
        }
        else
        {
            LOG_NET("TCP tcp_close graceful close");
        }

        LOG_NET("TCP pcb safely closed");

    } while(0);

    // 无论是否执行关闭逻辑，最终一定释放互斥锁和核心锁
    tcp_pending_close = 0;
    xSemaphoreGiveRecursive(tcp_send_mutex);
    UNLOCK_TCPIP_CORE();

    // 清空堆积发送报文，释放堆内存（无需持有核心锁，避免阻塞tcpip线程）
    tcp_send_msg_t drop;
    uint32_t drop_count = 0;
    while(xQueueReceive(tcp_send_queue, &drop, 0) == pdPASS)
    {
        if(drop.data != NULL)
        {
            vPortFree(drop.data);
            drop_count++;
            drop.data = NULL;
        }
    }
    if(drop_count > 0 || tcp_pcb != NULL)
    {
        LOG_NET("TCP clear %lu pending send msg", drop_count);
    }

    // 让出CPU给lwip tcpip内核线程，完成异步资源回收
    vTaskDelay(pdMS_TO_TICKS(20));
}


void Tcp_client_task(const void * argument)
{
    static uint32_t stable_tick = 0;
    static uint8_t s_last_link_up = 1;      // 上次物理链路状态，用于边沿检测
    const uint32_t stable_delay = 2000; // IP生效后延时2秒再连接
    const uint32_t reconnect_interval = 5000; // 失败后5秒才能再次重连

    while(1)
    {
        // ============================================================
        // 物理链路感知（拔线立即断链 / 插线立即快速重连）
        // 原实现完全不看链路状态：拔线后 tcp_pcb 仍非 NULL 且 g_tcp_connected==1，
        // 只能等 lwIP TCP 重传超时（TCP_MAXRTX=12，指数退避，几十秒~数分钟）才触发
        // errf -> tcp_pending_close，表现就是"检测不到断线、不触发重连"。
        // netif_is_link_up() 仅读 netif->flags，无需 core lock，可在本任务安全调用。
        // ============================================================
        uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
        if(link_up != s_last_link_up)
        {
            s_last_link_up = link_up;
            if(!link_up)
            {
                LOG_NET("ETH link DOWN -> force TCP close\r\n");
                if(tcp_pcb != NULL)
                {
                    tcp_pending_close = 1;   // 交给下面的异步关闭分支处理
                }
            }
            else
            {
                LOG_NET("ETH link UP -> fast reconnect\r\n");
                stable_tick      = 0;   // 重新计 2s 稳定期
                reconnect_tick   = 0;
                g_fast_reconnect = 1;   // 跳过 5s 重连节流
            }
        }

        // ==================================================
        // 恢复旧版核心：只检查 tcp_pending_close 标记
        // 不读取 pcb->state，避免 lwIP 异步竞态
        // ==================================================
        if(tcp_pending_close)
        {
            safe_tcp_close(); // 内部已加锁
            reconnect_tick = HAL_GetTick(); // 重置重连间隔计时
            osDelay(100);
            continue;
        }

        if(g_network_configured == 0)// 网络未配置：清空状态、断开TCP、复位所有计时
        {
            stable_tick = 0;
            reconnect_tick = 0;
            safe_tcp_close(); // 统一安全关闭
            osDelay(500);
            continue;
        }

        // 链路断开期间：不尝试建链，等待恢复
        // （注意必须放在 tcp_pending_close 处理之后，否则断线时的异步关闭会被跳过）
        if(!link_up)
        {
            osDelay(200);
            continue;
        }

        // ============================================================
        // 连接存活检测已拆成两层，这里不再做任何应用层心跳：
        //   1) 物理断线（拔网线/对端掉电/PHY link down）
        //      → 由本任务顶部的 netif_is_link_up() 边沿检测负责（PHY 100ms 轮询，最快）
        //   2) 链路仍 up 但对端已死（上位机进程崩溃、不再响应）
        //      → 由 lwIP TCP Keepalive 负责（PCB 创建时已置 SOF_KEEPALIVE：
        //        空闲 10s 起探测、间隔 3s、连续 5 次无响应则 tcp_abort
        //        -> errf(tcp_error_callback) -> tcp_pending_close -> 自动重连）
        //
        // 原应用层心跳已整体删除：其发送代码( tcp_send_heartbeat )早已被注释，
        // 实际只剩一个"每 20s 递增计数、累计 2 次即强制断链"的定时器，
        // 而它唯一的清零点是"收到合法下行报文" —— 上位机静默时设备会被自己周期性踢下线。
        // ============================================================

        if(tcp_pcb == NULL)// TCP自动重连
        {
            if(stable_tick == 0)
            {
                stable_tick    = HAL_GetTick();
                reconnect_tick = HAL_GetTick();
            }

            // 等待网络参数稳定
            uint32_t now = HAL_GetTick();
            if((now - stable_tick < stable_delay) ||
               ((now - reconnect_tick < reconnect_interval) && (g_fast_reconnect == 0)))
            {
                osDelay(100);
                continue;
            }
            g_fast_reconnect = 0; // 本次重连跳过节流，后续恢复正常节流

            // 在lwIP核心线程保护下创建/连接PCB，防止与tcpip_thread并发操作pcb链表
            LOCK_TCPIP_CORE();
            // 二次加锁防护，防止多路径创建PCB冲突
            if(tcp_pcb != NULL)
            {
                UNLOCK_TCPIP_CORE();
                continue;
            }
            struct tcp_pcb *new_pcb = tcp_new();//临时变量创建，失败不污染全局
            if(new_pcb != NULL)
            {
                // 绑定错误回调
                new_pcb->errf = tcp_error_callback;

                /* 启用 TCP Keepalive，替代已移除的应用层心跳。
                 * 原应用层心跳有两个问题：① 心跳包发送代码早已被注释，实际只剩一个
                 * "每 HEARTBEAT_PERIOD_MS 递增计数、累计 HB_LOST_MAX 次就强制断链"的
                 * 自伤定时器 —— 上位机不下发报文时设备会周期性把自己踢下线；
                 * ② 即便真发心跳，也只能证明"本机发出去了"，无法区分链路死与对端死。
                 * 交给 lwIP 内核保活更可靠：空闲 keep_idle 后开始发探测段（空 ACK），
                 * 连续 keep_cnt 次无响应则 tcp_abort -> errf -> tcp_pending_close -> 重连。
                 * 注意：pcb->tmr 在收到对端任何报文时都会被刷新，所以有正常下行数据时
                 * 不会误触发保活。此处处于 LOCK_TCPIP_CORE() 内，操作 PCB 是安全的。 */
                ip_set_option(new_pcb, SOF_KEEPALIVE);
#if LWIP_TCP_KEEPALIVE
                new_pcb->keep_idle  = TCP_KEEPIDLE_DEFAULT;   // 空闲多久后开始探测
                new_pcb->keep_intvl = TCP_KEEPINTVL_DEFAULT;  // 探测间隔
                new_pcb->keep_cnt   = TCP_KEEPCNT_DEFAULT;    // 探测次数，超时则 abort
#endif

                LOG_NET("Start connect server IP:%d.%d.%d.%d PORT:%d ......\r\n",
                    ip4_addr1(&g_server_ip),
                    ip4_addr2(&g_server_ip),
                    ip4_addr3(&g_server_ip),
                    ip4_addr4(&g_server_ip),
                    TCP_SERVER_PORT);

                err_t ret = tcp_connect(new_pcb, &g_server_ip, TCP_SERVER_PORT, tcp_connected_cb);
                if(ret != ERR_OK)
                {
                    tcp_abort(new_pcb);
                    new_pcb = NULL;
                    g_tcp_connected = 0;
                    stable_tick = 0;
                    reconnect_tick = HAL_GetTick();
                    UNLOCK_TCPIP_CORE();
                    osDelay(1500);
                }
                else
                {
                    tcp_pcb = new_pcb;//连接请求成功再赋值全局
                    UNLOCK_TCPIP_CORE();
                }
            }
            else // tcp_new 分配失败，直接重置重连计时，等待内存池回收
            {
                LOG_NET("tcp_new() failed, out of memory!\r\n");
                reconnect_tick = HAL_GetTick();
                UNLOCK_TCPIP_CORE();
                osDelay(2000); // 强制延时2秒，给内存池回收时间
            }
        }

        osDelay(10);
    }
}

/**
 * @brief TCP 错误回调（被动断连/连接失败）无锁直写volatile标记，无丢异常风险
 */
static void tcp_error_callback(void *arg, err_t err)
{
    (void)arg;
    // Cortex-M7单字节volatile原子写入，不使用互斥锁，不会丢失断开信号
    tcp_pending_close = 1;
    // errf返回后lwIP会立即释放该pcb，必须在此处清空全局指针，
    // 防止后续safe_tcp_close对已经释放的pcb再次调用tcp_abort造成HardFault/pbuf损坏
    tcp_pcb = NULL;
    g_tcp_connected = 0;
    LOG_NET("TCP error (mark async close), err:%d", err);
}

// TCP 连接成功回调
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if(err != ERR_OK)
    {
        // 10ms超时锁，避免lwIP内核卡死
        if(xSemaphoreTakeRecursive(tcp_send_mutex, pdMS_TO_TICKS(10)) == pdPASS)
        {
            if(tpcb != NULL && tcp_pcb == tpcb)
            {
                /* lwIP 回调里 pcb 所有权已归 lwIP，用户不得再 abort/free。
                   仅清空全局引用，lwIP 会通过 errf (tcp_error_callback) 释放 pcb。 */
                tcp_pcb = NULL;
                g_tcp_connected = 0;
                reconnect_tick = HAL_GetTick();
            }
            xSemaphoreGiveRecursive(tcp_send_mutex);
        }

        tcp_pending_close = 1;
        LOG_NET("TCP handshake fail, direct clean\r\n");
        return err;
    }

    // 握手成功：volatile 单变量无锁赋值，原子操作不会丢失
    tcp_pcb = tpcb;
    g_tcp_connected = 1;
    // 保活由 lwIP 内核接管（PCB 创建时已置 SOF_KEEPALIVE），此处无需重置任何应用层计数

    LOG_NET("TCP connect success!\r\n");

    // 修复：使用 lwIP 原生 API 关闭 Nagle
    tcp_nagle_disable(tpcb);

    // 注册接收回调
    tcp_recv(tpcb, tcp_recv_cb);

    // 注册发送回调（lwip内部发送通知）
    tcp_send_register();

    // 上电首次连接，标记需要上报重启结果
    if (g_reboot_result_reported == 0)
    {
        g_need_report_reboot_result = 1;
    }

    // 重启结果上报判断
    if (g_need_report_reboot_result == 1)
    {
        reboot_result_check_and_report();
        g_need_report_reboot_result = 0;
        g_reboot_result_reported = 1; // 本次上电永久锁定，不再触发
    }

    return ERR_OK;
}


// TCP 接收回调
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if(err != ERR_OK || p == NULL)
    {
        tcp_pending_close = 1;
        // 连接已关闭/出错，返回后lwIP会释放pcb，清空全局指针避免后续UAF
        tcp_pcb = NULL;
        g_tcp_connected = 0;
        return ERR_CLSD;
    }

    memset(tcp_rx_buf, 0, RX_BUF_SIZE);
    // p 可能是多段 pbuf 链，必须按 tot_len 处理，否则尾部段被丢弃且接收窗口少报
    uint16_t recv_len = (p->tot_len < (RX_BUF_SIZE - 1)) ? p->tot_len : (RX_BUF_SIZE - 1);
    pbuf_copy_partial(p, tcp_rx_buf, recv_len, 0);

    tcp_parse_cmd(tpcb, tcp_rx_buf);
    // 通知lwIP接收缓冲区已处理，释放pbuf内存（按整条链长度通告，避免窗口枯竭）
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}


// 解析网关下发指令
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf)
{
    char *sign_src = NULL;    // 统一指向签名字符串
    cJSON *temp_sign = NULL;
    HMAC_SHA256_CTX hmac;
    RebootCmd_t reboot_cmd = {0};
    /* upgrade_start 解析临时变量 */
    uint32_t ota_new_len = 0U;
    uint32_t ota_new_crc32 = 0U;
    /* sensor_data_ack 解析临时变量 */
    uint8_t ack_sensor_id = 0U;
    uint8_t ack_status = 0U;
    /* mode_set_req 解析临时变量 */
    uint8_t req_sensor_id = 0U;
    uint8_t req_mode = 0U;
    uint8_t req_persistent = 0U;

    LOG_NET("TCP Recv JSON:\r\n%s\r\n", buf);
    cJSON *root = cJSON_Parse(buf);
    if(!root) return;

    cJSON *type    = cJSON_GetObjectItem(root, "type");
    cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    cJSON *seq     = cJSON_GetObjectItem(root, "seq");
    cJSON *ts      = cJSON_GetObjectItem(root, "ts");
    cJSON *sign    = cJSON_GetObjectItem(root, "sign");

    if(!type || type->type != cJSON_String)            goto exit;
    if(!device_id || device_id->type != cJSON_String)  goto exit;
    if(!seq || seq->type != cJSON_Number)              goto exit;
    if(!ts || ts->type != cJSON_Number)                goto exit;
    if(!sign || sign->type != cJSON_String)            goto exit;

    if(strcmp(device_id->valuestring, g_device_id) != 0)
    {
        LOG_NET("Device ID verification failed!\r\n");
        goto exit;
    }

    uint32_t curr_seq = (uint32_t)seq->valuedouble;
    uint64_t curr_ts  = (uint64_t)ts->valuedouble;

    uint64_t now_ts = Get_Unix_Second();
    int64_t time_diff = llabs((int64_t)now_ts - (int64_t)curr_ts);
    if(time_diff > TIME_VALID_SEC)
    {
        LOG_NET("Time verification failed!\r\n");
        // goto exit;
    }

    if(curr_seq == g_last_cmd_seq)
    {
        LOG_NET("Sequence number verification failed!\r\n");
        // goto exit;
    }
    g_last_cmd_seq = curr_seq;

    char ts_cmd_str[21] = {0};
    uint64_to_str(curr_ts, ts_cmd_str);

    memset(tcp_sign_buf_2048, 0, sizeof(tcp_sign_buf_2048));

    // [2026-09-09 已移除] heartbeat_ack 被动响应分支。
    // 应用层心跳整体废弃：连接存活检测改由「链路层 netif_is_link_up 边沿检测
    // + lwIP TCP Keepalive(SOF_KEEPALIVE)」两层负责，设备不再发送也不需要应答心跳。
    // 上位机若仍下发 heartbeat_ack，将落入后续未知类型分支，不影响其他业务。
    if(strcmp(type->valuestring, "device_reboot") == 0)//解析 device_reboot 指令
    {
        cJSON *delay_obj = cJSON_GetObjectItem(root, "delay");
        cJSON *reason_obj = cJSON_GetObjectItem(root, "reason");

        if(delay_obj && delay_obj->type == cJSON_Number)
        {
            reboot_cmd.delay_sec = (uint32_t)delay_obj->valuedouble;
        }
        if(reason_obj && reason_obj->type == cJSON_String)
        {
            strncpy(reboot_cmd.reason, reason_obj->valuestring, sizeof(reboot_cmd.reason)-1);
            reboot_cmd.reason[sizeof(reboot_cmd.reason)-1] = '\0';
        }

        temp_sign = cJSON_CreateObject();
        if(!temp_sign) goto exit;

        if(delay_obj && delay_obj->type == cJSON_Number)
        {
            cJSON_AddNumberToObject(temp_sign, "delay", reboot_cmd.delay_sec);
        }
        cJSON_AddStringToObject(temp_sign, "device_id", g_device_id);
        if(reason_obj && reason_obj->type == cJSON_String)
        {
            cJSON_AddStringToObject(temp_sign, "reason", reboot_cmd.reason);
        }
        cJSON_AddNumberToObject(temp_sign, "seq", curr_seq);
        cJSON_AddNumberToObject(temp_sign, "ts", curr_ts);
        cJSON_AddStringToObject(temp_sign, "type", "device_reboot");

        sign_src = cJSON_PrintUnformatted(temp_sign);
        if(sign_src == NULL)
        {
            goto exit;
        }
    }

    else if(strcmp(type->valuestring, "upgrade_start") == 0)
    {
        cJSON *obj_new_len   = cJSON_GetObjectItem(root, "new_len");
        cJSON *obj_new_crc32 = cJSON_GetObjectItem(root, "new_crc32");

        if(!obj_new_len   || obj_new_len->type != cJSON_Number ||
           !obj_new_crc32 || obj_new_crc32->type != cJSON_Number)
        {
            LOG_NET("upgrade_start param invalid\r\n");
            goto exit;
        }

        ota_new_len   = (uint32_t)obj_new_len->valuedouble;
        ota_new_crc32 = (uint32_t)obj_new_crc32->valuedouble;

        /* HMAC签名源串，严格字典序，与网关完全对齐 */
        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"new_crc32\":%lu,\"new_len\":%lu,\"seq\":%lu,\"ts\":%s,\"type\":\"upgrade_start\"}",
            g_device_id,
            (unsigned long)ota_new_crc32,
            (unsigned long)ota_new_len,
            (unsigned long)curr_seq,
            ts_cmd_str);

        sign_src = tcp_sign_buf_2048;
    }
    /* ==========新增 sensor_data_ack 解析分支（网关→设备）========== */
    else if(strcmp(type->valuestring, "sensor_data_ack") == 0)
    {
        cJSON *obj_sensor_id = cJSON_GetObjectItem(root, "sensor_id");
        cJSON *obj_status    = cJSON_GetObjectItem(root, "status");

        if(!obj_sensor_id || obj_sensor_id->type != cJSON_Number ||
           !obj_status    || obj_status->type != cJSON_Number)
        {
            LOG_NET("sensor_data_ack param invalid\r\n");
            goto exit;
        }

        ack_sensor_id = (uint8_t)obj_sensor_id->valuedouble;
        ack_status    = (uint8_t)obj_status->valuedouble;

        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"sensor_id\":%d,\"seq\":%lu,\"status\":%d,\"ts\":%s,\"type\":\"sensor_data_ack\"}",
            g_device_id,
            ack_sensor_id,
            (unsigned long)curr_seq,
            ack_status,
            ts_cmd_str);

        sign_src = tcp_sign_buf_2048;
    }
    /* ==========新增 mode_set_req 解析分支（网关→设备）========== */
    else if(strcmp(type->valuestring, "mode_set_req") == 0)
    {
        cJSON *obj_sensor_id = cJSON_GetObjectItem(root, "sensor_id");
        cJSON *obj_mode      = cJSON_GetObjectItem(root, "mode");
        cJSON *obj_persist   = cJSON_GetObjectItem(root, "persistent");

        if(!obj_sensor_id || obj_sensor_id->type != cJSON_Number ||
           !obj_mode      || obj_mode->type != cJSON_Number ||
           !obj_persist   || obj_persist->type != cJSON_Number)
        {
            LOG_NET("mode_set_req param invalid\r\n");
            goto exit;
        }

        req_sensor_id = (uint8_t)obj_sensor_id->valuedouble;
        req_mode      = (uint8_t)obj_mode->valuedouble;
        req_persistent = (uint8_t)obj_persist->valuedouble;

        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"mode\":%d,\"persistent\":%d,\"sensor_id\":%d,\"seq\":%lu,\"ts\":%s,\"type\":\"mode_set_req\"}",
            g_device_id,
            req_mode,
            req_persistent,
            req_sensor_id,
            (unsigned long)curr_seq,
            ts_cmd_str);

        sign_src = tcp_sign_buf_2048;
    }
    else
    {
        goto exit;
    }

    if(sign_src == NULL || strlen(sign_src) == 0)
    {
        goto exit;
    }
    uint8_t calc_hash[32] = {0};
    char calc_sign[9] = {0};
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)sign_src, strlen(sign_src));
    hmac_sha256_final(&hmac, calc_hash);
    snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X",
            calc_hash[0], calc_hash[1], calc_hash[2], calc_hash[3]);

    if(strcmp(sign->valuestring, calc_sign) != 0)
    {
        LOG_NET("sign check failed!\r\nExpected: %s, Actual: %s\r\n", calc_sign, sign->valuestring);
        goto exit;
    }
    //LOG_NET("TCP received message successfully!\r\n");

    // 注：此处原本是应用层心跳计数的唯一起清零点（hb_lost_cnt = 0）。
    // 现已移除应用层心跳 —— lwIP 收到对端任何报文都会自动刷新 pcb->tmr，
    // 保活计时因此自然重置，无需应用层再干预。

    if(strcmp(type->valuestring, "device_reboot") == 0)//重启命令：入队重启任务，延时执行
    {
        if(xQueueSend(xRebootCmdQueue, &reboot_cmd, 0) != pdPASS)
        {
            LOG_NET("Reboot cmd queue full, drop command\r\n");
        }
    }

    if(strcmp(type->valuestring, "upgrade_start") == 0)//OTA升级开始：调用ota_handle_upgrade_start()，返回0则发送升级开始ACK，否则发送拒绝ACK
    {
        int ota_ret = ota_handle_upgrade_start(ota_new_len, ota_new_crc32);
        if(ota_ret == 0)
        {
            tcp_send_upgrade_start_ack(0, "upgrade_start_accepted", 60U);
        }
        else
        {
            tcp_send_upgrade_start_ack(1, "upgrade_start_rejected", 0U);
        }
    }

    // 传感器数据上报确认：记录各传感器最近ACK状态，并主动下发一次查询
    if(strcmp(type->valuestring, "sensor_data_ack") == 0)//传感器数据上报确认：记录各传感器最近ACK状态，并主动下发一次查询
    {
        if(ack_sensor_id >= 1 && ack_sensor_id <= 8)
        {
            g_sensor_ack_status[ack_sensor_id] = ack_status;

            /* 收到上位机 ACK 后，立即向该 CH 口发一次多寄存器查询（功能码 04，0x0000 起 4 个寄存器）。
             * 查询结果经原 RX 路径解析后通过 tcp_send_sensor_data() 上报。 */
            int8_t qret = SensorHub_Query(ack_sensor_id);
            if(qret == 0)
            {
                LOG_NET("sensor_data_ack -> query CH%u OK\r\n", ack_sensor_id);
            }
            else
            {
                LOG_NET("sensor_data_ack -> query CH%u FAIL(%d)\r\n", ack_sensor_id, (int)qret);
            }
        }
        LOG_NET("sensor_data_ack sensor_id:%u status:%u\r\n", ack_sensor_id, ack_status);
    }

    // 模式切换请求：Modbus写保持寄存器 + 按需持久化 + 回复 mode_set_resp
    if(strcmp(type->valuestring, "mode_set_req") == 0)
    {
        uint8_t status    = MODE_SET_OK;
        uint8_t persisted = 0;

        if(req_mode != SENSOR_MODE_REG && req_mode != SENSOR_MODE_ACTIVE)
        {
            status = MODE_SET_INVALID;   // 1-无效模式
        }
        else
        {
            // sensor_id == 0 表示全部传感器
            if(req_sensor_id == 0)
            {
                for(uint8_t sid = 1; sid <= 8; sid++)
                {
                    int8_t wr = sensorhub_write_mode(sid, req_mode);
                    if(wr == 0)
                    {
                        g_sensor_mode[sid] = req_mode;
                    }
                    else if(wr == 3)
                    {
                        status = MODE_SET_OFFLINE;     // 3-传感器离线
                        break;
                    }
                    else
                    {
                        status = MODE_SET_WRITE_FAIL;  // 2-写入失败
                        break;
                    }
                }
            }
            else if(req_sensor_id >= 1 && req_sensor_id <= 8)
            {
                int8_t wr = sensorhub_write_mode(req_sensor_id, req_mode);
                if(wr == 0)
                {
                    g_sensor_mode[req_sensor_id] = req_mode;
                }
                else if(wr == 3)
                {
                    status = MODE_SET_OFFLINE;
                }
                else
                {
                    status = MODE_SET_WRITE_FAIL;
                }
            }
            else
            {
                status = MODE_SET_INVALID; // 非法sensor_id
            }

            // 切换成功且要求持久化 → 写入Flash
            if(status == MODE_SET_OK && req_persistent == 1)
            {
                sensor_mode_save_flash();
                persisted = 1;
            }
        }

        tcp_send_mode_set_resp(req_sensor_id, status, req_mode, persisted);
    }

exit:
    if(temp_sign != NULL)
    {
        cJSON_Delete(temp_sign);
    }
    if(sign_src != tcp_sign_buf_2048 && sign_src != NULL)
    {
        cJSON_PortFree(sign_src);
    }
    cJSON_Delete(root);
}



// TCP 连接成功后立即发送 register 注册报文
static void tcp_send_register(void)
{
    if(tcp_pcb == NULL || g_tcp_connected !=1)
    {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Get_Unix_Second();
    char mac_str[32] = {0};
    uint8_t *mac = gnetif.hwaddr;

    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memset(sign_buf_register, 0, sizeof(sign_buf_register));
    char ts_online_str[21] = {0};
    uint64_to_str(ts, ts_online_str);
    g_tcp_seq=1; //注册报文seq固定为1，后续正常递增
    snprintf(sign_buf_register, sizeof(sign_buf_register) - 1,
        "{\"device_id\":\"%s\",\"firmware_ver\":\"%s\",\"mac\":\"%s\",\"model\":\"%s\",\"seq\":%lu,\"ts\":%s,\"type\":\"register\"}",
        g_device_id,
        DEVICE_FW_VER,
        mac_str,
        DEVICE_MODEL,
        (unsigned long)g_tcp_seq,
        ts_online_str);
    //LOG_NET("TCP Sorted register JSON:\r\n%s\r\n", sign_buf_register);

    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac_local;
    hmac_sha256_init(&hmac_local, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac_local, (const uint8_t *)sign_buf_register, strlen(sign_buf_register));
    hmac_sha256_final(&hmac_local, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    cJSON_AddStringToObject(root, "type", "register");
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(root, "firmware_ver", DEVICE_FW_VER);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        // 使用队列投递，不再直接调用 tcp_send_str
        LOG_NET("Send register:\r\n%s\r\n", str);
        tcp_send_msg_t msg;
        msg.data = (char *)pvPortMalloc(strlen(str) + 1);
        if(msg.data) {
            strcpy(msg.data, str);
            if(xQueueSend(tcp_send_queue, &msg, 0) != pdPASS) {
                vPortFree(msg.data);          // 队列满，丢弃
                LOG_NET("TCP send queue full, drop register");
            }
        }
        cJSON_PortFree(str);
        g_tcp_seq++;
    }
    cJSON_Delete(root);
}

/**
 * @brief 发送设备重启响应 device_reboot_ack
 * @param status 0-接受 1-拒绝 2-设备忙碌
 * @param message 状态描述字符串
 * @param estimated_duration 预计重启耗时（秒）
 */
void tcp_send_device_reboot_ack(uint8_t status, const char *message, uint32_t estimated_duration)
{
    if(tcp_pcb == NULL || g_tcp_connected != 1)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Get_Unix_Second();

    // 顶层字段严格字典序：device_id → estimated_duration → message → seq → status → ts → type
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddNumberToObject(root, "estimated_duration", estimated_duration);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "type", "device_reboot_ack");

    // 生成无签名字符串，计算HMAC
    char *json_no_sign = cJSON_PrintUnformatted(root);
    if (json_no_sign == NULL)
    {
        cJSON_Delete(root);
        return;
    }

    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac_local;
    hmac_sha256_init(&hmac_local, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac_local, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac_local, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 末尾追加sign字段
    cJSON_AddStringToObject(root, "sign", sign_str);

    // 发送报文
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        // 使用队列投递，不再直接调用 tcp_send_str
        LOG_NET("Send device_reboot_ack:\r\n%s\r\n", str);
        tcp_send_msg_t msg;
        msg.data = (char *)pvPortMalloc(strlen(str) + 1);
        if(msg.data) {
            strcpy(msg.data, str);
            if(xQueueSend(tcp_send_queue, &msg, 0) != pdPASS) {
                vPortFree(msg.data);          // 队列满，丢弃
                LOG_NET("TCP send queue full, drop device_reboot_ack");
            }
        }
        cJSON_PortFree(str);   // 投递后再释放 cJSON 字符串
        g_tcp_seq++;
    }

    cJSON_PortFree(json_no_sign);
    cJSON_Delete(root);
}

/**
 * @brief 发送 OTA 升级启动响应 upgrade_start_ack
 * @param status 0-接受(已开始TFTP监听) 1-拒绝(忙碌/参数/分区/初始化失败)
 * @param message 状态描述字符串
 * @param estimated_duration 预计完成升级耗时（秒）
 */
void tcp_send_upgrade_start_ack(uint8_t status, const char *message, uint32_t estimated_duration)
{
    if(tcp_pcb == NULL || g_tcp_connected != 1)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Get_Unix_Second();

    // 顶层字段严格字典序：device_id → estimated_duration → message → seq → status → ts → type
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddNumberToObject(root, "estimated_duration", estimated_duration);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "type", "upgrade_start_ack");

    // 生成无签名字符串，计算HMAC
    char *json_no_sign = cJSON_PrintUnformatted(root);
    if (json_no_sign == NULL)
    {
        cJSON_Delete(root);
        return;
    }

    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac_local;
    hmac_sha256_init(&hmac_local, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac_local, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac_local, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 末尾追加sign字段
    cJSON_AddStringToObject(root, "sign", sign_str);

    // 发送报文
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        // 使用队列投递，不再直接调用 tcp_send_str
        LOG_NET("Send upgrade_start_ack:\r\n%s\r\n", str);
        tcp_send_msg_t msg;
        msg.data = (char *)pvPortMalloc(strlen(str) + 1);
        if(msg.data) {
            strcpy(msg.data, str);
            if(xQueueSend(tcp_send_queue, &msg, 0) != pdPASS) {
                vPortFree(msg.data);          // 队列满，丢弃
                LOG_NET("TCP send queue full, drop upgrade_start_ack");
            }
        }
        cJSON_PortFree(str);   // 投递后再释放 cJSON 字符串
        g_tcp_seq++;
    }

    cJSON_PortFree(json_no_sign);
    cJSON_Delete(root);
}

void tcp_send_device_reboot_result(uint8_t result, const char *message, const char *reason, uint32_t uptime)
{
    if(tcp_pcb == NULL || g_tcp_connected != 1)
    {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Get_Unix_Second();

    // 严格字典序：device_id → message → reason → result → seq → ts → type → uptime
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "reason", reason);
    cJSON_AddNumberToObject(root, "result", result);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "type", "device_reboot_result");
    cJSON_AddNumberToObject(root, "uptime", uptime);

    char *json_no_sign = cJSON_PrintUnformatted(root);
    if (!json_no_sign)
    {
        cJSON_Delete(root);
        return;
    }

    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac_local;
    hmac_sha256_init(&hmac_local, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac_local, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac_local, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X", hash[0], hash[1], hash[2], hash[3]);
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *send_buf = cJSON_PrintUnformatted(root);
    if(send_buf)
    {
        LOG_NET("Send device_reboot_result:\r\n%s\r\n", send_buf);
        // 使用队列投递，不再直接调用 tcp_send_str
        tcp_send_msg_t msg;
        msg.data = (char *)pvPortMalloc(strlen(send_buf) + 1);
        if(msg.data) {
            strcpy(msg.data, send_buf);
            if(xQueueSend(tcp_send_queue, &msg, 0) != pdPASS) {
                vPortFree(msg.data);          // 队列满，丢弃
                LOG_NET("TCP send queue full, drop device_reboot_result");
            }
        }
        cJSON_PortFree(send_buf);
        g_tcp_seq++;
    }
    cJSON_PortFree(json_no_sign);
    cJSON_Delete(root);
}

//======================== 传感器数据上报 =============================
/**
 * @brief 发送传感器数据报文 sensor_data（集中器 → 网关）
 * @note  须在任务上下文中调用（内部使用cJSON/pvPortMalloc/HMAC，非ISR安全）。
 *        集中器在中断收到终端传感器数据后，应先将数据通过队列投递到任务，再调用本函数。
 */
void tcp_send_sensor_data(const SensorData_t *sd)
{
    if(tcp_pcb == NULL || g_tcp_connected != 1)
    {
        return;
    }
    if(sd == NULL)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if(root == NULL)
    {
        return;
    }
    uint64_t ts = Get_Unix_Second();

    // 顶层字段严格字典序：device_id → hw_version → model → mode → sensor_id →
    // seq → sw_version → temp_flag → temperature → ts → type → voltage → voltage_flag
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddStringToObject(root, "hw_version", DEVICE_HW_VER);
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddNumberToObject(root, "mode", sd->mode);
    cJSON_AddNumberToObject(root, "sensor_id", sd->sensor_id);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddStringToObject(root, "sw_version", DEVICE_FW_VER);
    cJSON_AddNumberToObject(root, "temp_flag", sd->temp_flag);
    cJSON_AddNumberToObject(root, "temperature", sd->temperature);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "type", "sensor_data");
    cJSON_AddNumberToObject(root, "voltage", sd->voltage);
    cJSON_AddNumberToObject(root, "voltage_flag", sd->voltage_flag);

    // 生成无签名字符串，计算HMAC
    char *json_no_sign = cJSON_PrintUnformatted(root);
    if(json_no_sign == NULL)
    {
        cJSON_Delete(root);
        return;
    }

    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac_local;
    hmac_sha256_init(&hmac_local, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac_local, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac_local, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 末尾追加sign字段
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG_NET("Send sensor_data:\r\n%s\r\n", str);
        tcp_send_msg_t msg;
        msg.data = (char *)pvPortMalloc(strlen(str) + 1);
        if(msg.data) {
            strcpy(msg.data, str);
            if(xQueueSend(tcp_send_queue, &msg, 0) != pdPASS) {
                vPortFree(msg.data);          // 队列满，丢弃
                LOG_NET("TCP send queue full, drop sensor_data");
            }
        }
        cJSON_PortFree(str);
        g_tcp_seq++;
    }

    cJSON_PortFree(json_no_sign);
    cJSON_Delete(root);
}


//======================== 模式切换响应 =============================
/**
 * @brief 发送模式切换响应 mode_set_resp（集中器 → 网关）
 * @param sensor_id 传感器ID（0表示全部）
 * @param status    0-成功 1-无效模式 2-写入失败 3-传感器离线
 * @param mode      应用的模式
 * @param persisted 1-已持久化 0-未持久化
 */
static void tcp_send_mode_set_resp(uint8_t sensor_id, uint8_t status, uint8_t mode, uint8_t persisted)
{
    if(tcp_pcb == NULL || g_tcp_connected != 1)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if(root == NULL)
    {
        return;
    }
    uint64_t ts = Get_Unix_Second();

    // 顶层字段严格字典序：device_id → mode → persisted → sensor_id → seq → status → ts → type
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddNumberToObject(root, "mode", mode);
    cJSON_AddNumberToObject(root, "persisted", persisted);
    cJSON_AddNumberToObject(root, "sensor_id", sensor_id);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "type", "mode_set_resp");

    char *json_no_sign = cJSON_PrintUnformatted(root);
    if(json_no_sign == NULL)
    {
        cJSON_Delete(root);
        return;
    }

    uint8_t hash[32] = {0};
    HMAC_SHA256_CTX hmac_local;
    hmac_sha256_init(&hmac_local, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac_local, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac_local, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG_NET("Send mode_set_resp:\r\n%s\r\n", str);
        tcp_send_msg_t msg;
        msg.data = (char *)pvPortMalloc(strlen(str) + 1);
        if(msg.data) {
            strcpy(msg.data, str);
            if(xQueueSend(tcp_send_queue, &msg, 0) != pdPASS) {
                vPortFree(msg.data);          // 队列满，丢弃
                LOG_NET("TCP send queue full, drop mode_set_resp");
            }
        }
        cJSON_PortFree(str);
        g_tcp_seq++;
    }

    cJSON_PortFree(json_no_sign);
    cJSON_Delete(root);
}


// TCP NTP时间同步线程函数
void TimeSyncTask(const void * argument)
{
    char ntp_time_buf[32] = {0};
    char rtc_time_buf[32] = {0};
    char diff_buf[24] = {0};
    char ts_str[21]  = {0};
    char ts_buf[24] = {0};
    char ntp_ip_buf[20] = "";

    const uint64_t TIME_DIFF_THRESHOLD = 2000;  // 偏差阈值 2秒
    const uint32_t CHECK_INTERVAL      = 10000;  // 校验周期 10秒
    static uint32_t last_check_tick    = 0;
    static uint8_t  first_sync_done    = 0;     // 上电首次同步标记
    uint64_t rtc_ms = 0;
    uint64_t diff = 0;
    for(;;)
    {
        uint32_t now_tick = HAL_GetTick();
        if(now_tick - last_check_tick >= CHECK_INTERVAL)
        {
            // 读取失败：直接保留上一次缓存值，不做任何修改
            // ======================================================================
            sys_unix_ms = 0;
            NTP_Force_Refresh();//主动刷新一次NTP时间，获取最新的时间戳用于打印（不依赖被动同步回调）
            osDelay(1000);//NTP发包收包需要一小段等待，不能立刻打印
            if(sys_unix_ms > 0)
            {
                rtc_ms = RTC_To_UnixMs();
                if(sys_unix_ms > rtc_ms)
                {
                    diff = sys_unix_ms - rtc_ms;
                }
                else
                {
                    diff = rtc_ms - sys_unix_ms;
                }

                // 上电首次 或 差值超限 执行同步
                if(first_sync_done == 0 || diff > TIME_DIFF_THRESHOLD)
                {
                    UnixMs_To_RTC(sys_unix_ms);

                    if(first_sync_done == 0)
                    {
                        first_sync_done = 1;
                        LOG_NET("Power-on first sync NTP -> RTC OK");
                    }
                    else
                    {
                        uint64_to_str_2(diff, diff_buf, sizeof(diff_buf));
                        LOG_NET("Time diff: %s ms > threshold, re-sync", diff_buf);
                    }
                }
            }
            // 格式化NTP服务端IP字符串
            const ip_addr_t *p_current_ntp = sntp_getserver(0);
            snprintf(ntp_ip_buf, sizeof(ntp_ip_buf), "%d.%d.%d.%d",
                    ip4_addr1(p_current_ntp),
                    ip4_addr2(p_current_ntp),
                    ip4_addr3(p_current_ntp),
                    ip4_addr4(p_current_ntp));
            LOG_NET("Current NTP Server IP: %s", ntp_ip_buf);
            // 打印NTP时间戳和RTC时间戳，观察两者是否一致（误差在TIME_DIFF_THRESHOLD毫秒内，符合预期）
            if(sys_unix_ms > 0)
            {
                stamp_to_time(sys_unix_ms, ntp_time_buf, sizeof(ntp_time_buf));
                uint64_to_str_2(sys_unix_ms, ts_str, sizeof(ts_str));
                LOG_NET("NTPTime: %s | MS_TS: %s", ntp_time_buf, ts_str);
            }
            else
            {
                LOG_NET("NTP sync fail, ts=0");
            }
            //RTC时间打印，观察是否与NTP时间一致
            rtc_ms = RTC_To_UnixMs();
            stamp_to_time(rtc_ms, rtc_time_buf, sizeof(rtc_time_buf));
            uint64_to_str_2(rtc_ms, ts_buf, sizeof(ts_buf));
            LOG_NET("RTCTime: %s | MS_TS: %s\r\n", rtc_time_buf, ts_buf);
            last_check_tick = now_tick;
        }
        osDelay(100); // 小延时，让出CPU
    }
}


/**
 * @brief  重启处理专属任务
 */
void vRebootTask(const void * argument)
{
    RebootCmd_t cmd;
    for(;;)
    {
        // 阻塞等待重启指令
        if(xQueueReceive(xRebootCmdQueue, &cmd, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        // 设备已空闲，接受重启，发送应答告知上位机
        cmd.delay_sec = 0;//强制延迟0秒，立即重启
        tcp_send_device_reboot_ack(0, "reboot_accepted", Initialization_time + cmd.delay_sec);
        LOG_NET("Accept reboot: delay = %us, reason = %s\r\n", (unsigned int)cmd.delay_sec, cmd.reason);

        // 5. 写入掉电存储，标记主动重启
        reboot_save_persistent_info(cmd.reason);

        // 3. 等待应答报文发送完成
        vTaskDelay(pdMS_TO_TICKS(500));

        // 4. 延迟指定秒数
        if(cmd.delay_sec > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(cmd.delay_sec * 1000));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        
        // 6. 关闭总中断，执行系统软复位
        __set_FAULTMASK(1);
        HAL_NVIC_SystemReset();
        while(1); // 兜底死循环
    }
}


/**
 @brief TCP异步发送任务
 send_buf 维持 2048 字节
 移除 tcp_output，依赖 LwIP 内部定时器发送
 增加严格的空指针和长度检查
 兼容C89标准，大数组定义在函数头部，消除size_t类型转换警告
 */
void tcp_sender_task(const void * argument)
{
    (void)argument;
    tcp_send_msg_t msg;

    // C89标准：局部大数组定义在函数最开头，仅占用一次栈空间
    char send_buf[2048];

    const uint32_t MAX_DROP_CNT = 16U; // 单次最多丢弃16条，防止死循环
    const uint32_t QUEUE_ALERT_THRESHOLD = 20U; // 队列堆积20条提前限流

    while(1)
    {
        // 1. 阻塞等待队列消息
        if(xQueueReceive(tcp_send_queue, &msg, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        // 2. 基础空指针校验
        if(msg.data == NULL)
        {
            continue;
        }

        // ==================================================
        // 关键修复：报文预处理全部锁外执行，减少持锁时间
        // ==================================================
        size_t json_len_size = strlen(msg.data);
        if(json_len_size >= sizeof(send_buf))
        {
            LOG_NET("TCP msg too long (%u), drop\r\n", (unsigned int)json_len_size);
            vPortFree(msg.data);
            continue;
        }

        u16_t json_len = (u16_t)json_len_size;
        u16_t send_len = json_len + 1; // 额外存放换行符

        // 拷贝报文并追加换行符
        memcpy(send_buf, msg.data, json_len);
        send_buf[json_len] = '\n';

        // 队列堆积预警，锁外先清理一部分，减少锁内耗时
        uint32_t pending = uxQueueMessagesWaiting(tcp_send_queue);
        if(pending > QUEUE_ALERT_THRESHOLD)
        {
            tcp_send_msg_t drop_msg;
            uint32_t clear = 0;
            while((xQueueReceive(tcp_send_queue, &drop_msg, 0) == pdPASS) && clear < MAX_DROP_CNT)
            {
                if(drop_msg.data)
                {
                    vPortFree(drop_msg.data);
                }
                clear++;
            }
            LOG_NET("Queue overflow pre clear %lu pkt\r\n", clear);
        }

        // ==================================================
        // 临界区：先做状态校验、发送空间校验、写入，再解锁
        // 必须先获取lwIP核心锁，再拿tcp_send_mutex，顺序与safe_tcp_close一致(core->mutex)
        // ==================================================
        LOCK_TCPIP_CORE();
        xSemaphoreTakeRecursive(tcp_send_mutex, osWaitForever);

        // TCP未连接，丢弃消息释放内存
        if(tcp_pcb == NULL || g_tcp_connected != 1)
        {
            LOG_NET("TCP offline, drop message\r\n");
            vPortFree(msg.data);
            xSemaphoreGiveRecursive(tcp_send_mutex);
            UNLOCK_TCPIP_CORE();
            continue;
        }

        // 校验lwIP发送缓冲区剩余空间
        u16_t free_buf = tcp_sndbuf(tcp_pcb);
        // 缓存不足，锁内最多清空MAX_DROP_CNT条，避免死循环
        uint32_t drop_cnt = 0;
        while((free_buf < send_len) && (drop_cnt < MAX_DROP_CNT))
        {
            tcp_send_msg_t drop_msg;
            if(xQueueReceive(tcp_send_queue, &drop_msg, 0) == pdPASS)
            {
                if(drop_msg.data != NULL)
                {
                    vPortFree(drop_msg.data);
                    LOG_NET("TCP buf full, drop pending pkt\r\n");
                }
                drop_cnt++;
            }
            else
            {
                break;
            }
            free_buf = tcp_sndbuf(tcp_pcb);
        }

        if(free_buf < send_len)
        {
            LOG_NET("TCP send buf full (need %d, free %d), drop pkt\r\n", send_len, free_buf);
            vPortFree(msg.data);
            xSemaphoreGiveRecursive(tcp_send_mutex);
            UNLOCK_TCPIP_CORE();
            continue;
        }

        // 3. 写入 lwIP 发送缓存
        err_t ret = tcp_write(tcp_pcb, send_buf, send_len, TCP_WRITE_FLAG_COPY);
        // 4. 关键恢复：强制推送报文到网络，否则数据可能卡在缓存里发不出去
        if(ret == ERR_OK)
        {
            //tcp_output(tcp_pcb);
            vPortFree(msg.data);
            xSemaphoreGiveRecursive(tcp_send_mutex);
            UNLOCK_TCPIP_CORE();
        }
        else
        {
            LOG_NET("tcp_write fail, err:%d, mark async close\r\n", ret);
            tcp_pending_close = 1;
            // 失败提前释放、解锁、跳出循环，不再执行下方代码
            vPortFree(msg.data);
            xSemaphoreGiveRecursive(tcp_send_mutex);
            UNLOCK_TCPIP_CORE();
            continue;
        }
    }
}


