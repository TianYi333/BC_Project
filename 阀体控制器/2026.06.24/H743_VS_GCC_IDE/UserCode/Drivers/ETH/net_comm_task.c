#include "net_comm_task.h"
#include "syncif.h"

//==== 全局静态缓冲区，替代函数内局部大数组 ====
static char sign_buf_req[512];          // 专门给 request 验签用
static char sign_buf_resp[512];         // 专门给 response 组包用
static char sign_buf_online[512];       // 专门给 online 组包用
static char sign_buf_register[512];     // 专门给 register 组包用
// static char tcp_sign_buf_1024[1024];
static char tcp_sign_buf_2048[2048];

static char tcp_rx_buf[RX_BUF_SIZE];
static struct tcp_pcb *tcp_pcb = NULL;
static uint32_t g_tcp_seq = 1;                      //tcp全局自增序列号，上电从 1 开始
static uint32_t g_udp_seq = 1;                      //udp全局自增序列号，上电从 1 开始
volatile static uint32_t g_last_cfg_seq = 0;        //上位机 -> 设备：UDP报文 seq
volatile static uint32_t g_last_cmd_seq = 0;        //上位机 -> 设备：TCP报文 seq 
volatile static uint8_t  g_network_configured = 0;  //配网成功标志：0=未配网 1=已配网
uint8_t g_is_reboot = 1;                            //设备上电状态标记：true=重启/非首次上电  false=首次上电(无历史配网)
static uint32_t reconnect_tick = 0;                 //TCP重连节流计时，避免频繁重连
static uint8_t  g_tcp_connected = 0;                //0=未真正连接 1=握手成功
static uint8_t sntp_inited = 0;                     //SNTP初始化标志
uint64_t sys_unix_ms= 0;                            //系统毫秒对时
static uint8_t hb_lost_cnt = 0;                     //心跳连续丢失计数（0=正常，累计3次断开重连）
static uint32_t last_hb_tick = 0;                   //心跳计时
static ip4_addr_t  g_server_ip;                     //上位机业务服务器IP（动态保存）
static NetConfig_t g_net_cfg;                       //网络配置结构体（固化到Flash）
struct fdb_kvdb net_kvdb;                           //网络独立数据库句柄
TaskInfo_t g_task_request_buf[TASK_DATA_MAX];       //任务请求数据缓冲区，最多支持8条任务
uint16_t g_task_request_cnt = 0;                    //记录当前有效元素个数
TaskOrderInfo_t g_task_order_buf[TASK_DATA_MAX];
uint16_t g_task_order_cnt = 0;                      //记录当前有效元素个数
ExecuteResultInfo_t g_task_execute_result_buf[TASK_DATA_MAX];// 任务执行结果缓冲区，最多支持8条任务
uint16_t g_task_execute_result_cnt = 0;             //记录当前有效元素个数
TaskBindInfo_t g_tcp_task_bind = {0};
volatile uint8_t g_force_stop_task = 0;             //强制停止标记
char g_stop_target_task_id[48] = {0};               //待停止的任务ID
HMAC_SHA256_CTX hmac;
ExecuteResultInfo_t exec_info;                      //任务结果上报
uint8_t g_current_running_inj_id = 0;               // 全局记录当前正在执行注油的出油口ID，0=无任务运行

void uint64_to_str_2(uint64_t num, char *str, size_t size);
static void udp_recv_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static void udp_send_device_online(struct udp_pcb *upcb, uint16_t reason);
static void udp_msg_process(const char *buf, const ip_addr_t *src_ip, u16_t src_port, struct udp_pcb *upcb);
static void tcp_error_callback(void *arg, err_t err);
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf);
static void tcp_send_str(struct tcp_pcb *tpcb, const char *str);
static void tcp_send_heartbeat(HeartBeatParam_t *p_param);
static void tcp_send_register(void);
void FillHeartBeatData(HeartBeatParam_t *hb_param);
void FillStateRespData(StateRespParam_t *resp_param, uint16_t port_idx);
void tcp_send_state_response(StateRespParam_t *p_param);
void tcp_send_task_request(TaskInfo_t *info_arr, uint16_t arr_len, uint8_t is_oiling);
void tcp_send_execute_result(ExecuteResultInfo_t *info_arr, uint16_t arr_len);
void Lwip_SNTP_Init(void);
void TimeSyncTask(void *arg);


void Lwip_SNTP_Init(void)
{
    ip_addr_t ntp_ip;
    IP4_ADDR(&ntp_ip,192,168,0,10);
    sntp_setserver(0, &ntp_ip); //标准入参：序号+ip_addr_t指针
    // 2.开启SNTP
    sntp_init();
}


int _write(int file, char *ptr, int len)
{
    if (uart_mutex == NULL) return len; // 锁未初始化时直接发送

    osMutexAcquire(uart_mutex, osWaitForever);
    uint16_t idx = 0;
    const uint16_t chunk_size = 128; // 每次最多发128字节，远小于UART FIFO压力
    while (idx < len)
    {
        uint16_t send_len = (len - idx) > chunk_size ? chunk_size : (len - idx);
        HAL_UART_Transmit(&huart2, (uint8_t *)(ptr + idx), send_len, HAL_MAX_DELAY);
        idx += send_len;
    }
    osMutexRelease(uart_mutex);
    return len;
}

// 打印本地IP地址（调试用）
void print_local_ip(void)
{
    char ip_buf[16];
    ipaddr_ntoa_r(&gnetif.ip_addr, ip_buf, sizeof(ip_buf));
    LOG("Local IP: %s", ip_buf);
}

// 网络数据库初始化（独立、安全、带线程锁）
int8_t init_net_db(void)
{
    fdb_err_t result;
    struct fdb_default_kv default_kv = {0};

    fdb_kvdb_control(&net_kvdb, FDB_KVDB_CTRL_SET_LOCK, (void *)lock);
    fdb_kvdb_control(&net_kvdb, FDB_KVDB_CTRL_SET_UNLOCK, (void *)unlock);

    result = fdb_kvdb_init(&net_kvdb, "netdb", "ef_kvdb2", &default_kv, NULL);

    if(result != FDB_NO_ERR) {
        LOG("ETH database initialization failed, err=%d", result);
        return -1;
    }
    LOG("ETH database initialization successful");
    return 0;
}


// 上电读取Flash保存的网络配置，自动配置网卡IP
void net_config_init(void)
{
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;

    memset(&g_net_cfg, 0, sizeof(NetConfig_t));

    blob->buf = (uint8_t *)&g_net_cfg;
    blob->size = sizeof(NetConfig_t);

    size_t len = fdb_kv_get_blob(&net_kvdb, "net_cfg", blob);

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

        netif_set_ipaddr(&gnetif, &ip);
        netif_set_netmask(&gnetif, &mask);
        netif_set_gw(&gnetif, &gw);

        autoip_stop(&gnetif);
        netif_set_down(&gnetif);
        netif_set_up(&gnetif);

        gnetif.flags |= NETIF_FLAG_BROADCAST;

        g_server_ip = srv_ip;
        g_network_configured = 1;
        LOG("Flash loading network configuration succeeded, TCP started");
        if(sntp_inited == 0)
        {
            Lwip_SNTP_Init();
            sntp_inited = 1;
        }
    }
    else
    {
        autoip_start(&gnetif);
        g_network_configured = 0;
        LOG("Not connected to the network, starting UDP broadcast");
    }
}


// 保存配网参数 to FlashDB
static void net_config_save(void)
{
    g_net_cfg.configured = 1;

    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;
    blob->buf = (uint8_t *)&g_net_cfg;
    blob->size = sizeof(NetConfig_t);

    fdb_kv_set_blob(&net_kvdb, "net_cfg", blob);

    LOG("The distribution network parameters have been saved to Flash");
}

void NTP_Force_Refresh(void)//快速手动刷新
{
    sntp_stop();
    sys_unix_ms = 0;
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

/**
 * @brief  UDP设备发现任务，端口50000（始终运行）
 * @note   监听上位机UDP广播发现报文，回复设备信息
 *         仅开启广播接收，AutoIP模式无法使用组播
 */
void udp_discover_task(void *arg)
{
    struct udp_pcb *upcb = NULL;
    ip4_addr_t local_ip;
    
    while(1)
    {
        local_ip = *netif_ip4_addr(&gnetif);
        if(!ip4_addr_isany(&local_ip))
        {
            break;
        }
        LOG("Waiting for auto link ip...");
        osDelay(1000);
    }
    print_local_ip();
    if(sntp_inited == 0)
    {
        Lwip_SNTP_Init();
        sntp_inited = 1;
        LOG("AutoIP OK, start SNTP");
    }
    LOG("Valid IP obtained, start UDP service");

    upcb = udp_new();
    if(upcb != NULL)
    {
        udp_bind(upcb, IP_ADDR_ANY, UDP_LISTEN_PORT);
        upcb->so_options |= SOF_BROADCAST;
        gnetif.flags |= NETIF_FLAG_BROADCAST;// 开启网口广播权限
        udp_recv(upcb, udp_recv_callback, NULL);

        LOG("UDP PCB created successfully");
        LOG("UDP Binding successful port: %d", upcb->local_port);
        LOG("SOF_BROADCAST enabled: %s",
            (upcb->so_options & SOF_BROADCAST) ? "YES" : "NO");
        LOG("netif flags: 0x%08X", gnetif.flags);
        osDelay(3000); // 短暂延时保证网络稳定
        udp_send_device_online(upcb,0);//上电
    }
    else
    {
        LOG("udp_new() failed, no pcb available!");
    }

    UdpMsgTypeDef udp_msg;
    
    while(1)
    {
        //osDelay(2000);
        // const char *pure_json_buf_2 = "{\"domain\":\"FACTORY_OIL\",\"gateway_mac\":\"1a:f5:79:d4:5e:32\",\"seq\":1,\"ts\":1776996714,\"type\":\"discover_request\"}";
        
        // LOG("Sorted JSON: %s\r\n", pure_json_buf_2);
        // LOG("Sorted JSON Length: %lu\r\n", (unsigned long)strlen(pure_json_buf_2));
        // uint8_t gw_calc_hash[32] = {0};

        // hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        // hmac_sha256_update(&hmac, (const uint8_t *)pure_json_buf_2, strlen(pure_json_buf_2));
        // hmac_sha256_final(&hmac, gw_calc_hash);

        // char gw_calc_sign[9] = {0};
        // snprintf(gw_calc_sign, sizeof(gw_calc_sign), "%02X%02X%02X%02X",
        //         gw_calc_hash[0], gw_calc_hash[1],
        //         gw_calc_hash[2], gw_calc_hash[3]);
        // LOG("Calc sign: %s\r\n", gw_calc_sign);
        if(xQueueReceive(udp_msg_queue, &udp_msg, pdMS_TO_TICKS(100)) == pdPASS)
        {
            char *buf = udp_msg.data;
            udp_msg_process(buf, &udp_msg.src_ip, udp_msg.src_port, upcb);
        }
        //循环读取 PHY 链路状态
        // int32_t link = LAN8742_GetLinkState(&LAN8742);
        // uint32_t flags = gnetif.flags;
        // LOG("LinkState:%d, NetIfFlags:0x%08X\r\n", link, flags);
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
    if(upcb == NULL) return;
    // 网卡未就绪直接退出，防止IP接口非法访问
    if(!gnetif.flags & NETIF_FLAG_UP)
    {
        LOG("netif not up, skip udp online broadcast\r\n");
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
            DEVICE_SN,
            ts_online_str,
            "device_online");
    //打印签名校验原文，调试用
    // LOG("UDP Sorted device_online JSON:\r\n%s\r\n", sign_buf_online);
    uint8_t hash[32] = {0};
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_online, strlen(sign_buf_online));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 组装完整报文
    cJSON_AddStringToObject(root, "type", "device_online");
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "sn", DEVICE_SN);
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
        LOG("UDP Send JSON:\r\n%s\r\n", str);
        uint16_t json_len = strlen(str);
        uint16_t send_len = json_len + 1;  // +1 存换行符
        struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, send_len, PBUF_POOL);
        if(send_len > 1472)
        {
            LOG("udp json too long, drop\r\n");
            free(str);
            cJSON_Delete(root);
            return;
        }
        if(p_tx != NULL)
        {

            memcpy(p_tx->payload, str, json_len);
            ((uint8_t *)p_tx->payload)[json_len] = '\n'; // 追加换行
            // UDP 广播发送（对应上位机发现端口 UDP_LISTEN_PORT）
            err_t ret = udp_sendto(upcb, p_tx, IP_ADDR_BROADCAST, UDP_LISTEN_PORT);
            LOG("UDP device_online send status:%d\r\n", ret);
            pbuf_free(p_tx);
        }
        else
        {
            // 内存池不足日志
            LOG("pbuf alloc failed, pool empty\r\n");
        }
        free(str);
        g_udp_seq++;
    }
    cJSON_Delete(root);
}


// UDP报文统一处理分发
static void udp_msg_process(const char *buf, const ip_addr_t *src_ip, u16_t src_port, struct udp_pcb *upcb)
{
    LOG("UDP Recv JSON:\r\n%s\r\n", buf);
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
            LOG("Time verification failed!\r\n");
            // goto udp_task_err;
        }
        uint32_t curr_gw_seq = (uint32_t)seq->valuedouble;
         if(curr_gw_seq == g_last_cfg_seq )
         {
            LOG("Sequence number verification failed!\r\n");
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

        //LOG("UDP Sorted discover_request JSON:\r\n%s\r\n", sign_buf_req);

        uint8_t gw_calc_hash[32] = {0};
        hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_req, strlen(sign_buf_req));
        hmac_sha256_final(&hmac, gw_calc_hash);

        char gw_calc_sign[9] = {0};
        snprintf(gw_calc_sign, sizeof(gw_calc_sign), "%02X%02X%02X%02X",
                gw_calc_hash[0], gw_calc_hash[1],
                gw_calc_hash[2], gw_calc_hash[3]);
        //LOG("Calc discover_request sign: %s\r\n", gw_calc_sign);      
        //签名不匹配 → 非法报文
        if (strcmp(sign->valuestring, gw_calc_sign) != 0) {
             LOG("sign check failed!\r\nExpected: %s, Actual: %s\r\n", gw_calc_sign, sign->valuestring);
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
            cJSON_AddStringToObject(resp, "sn", "OIL-2026-0001");
            cJSON_AddStringToObject(resp, "model", "LUB-CTRL-V1.0");
            cJSON_AddStringToObject(resp, "firmware_ver", "1.0.3");
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
                DEVICE_SN,
                ts_buf,
                "discover_response"
            );
            //LOG("UDP Sorted discover_response JSON:\r\n%s\r\n", sign_buf_resp);

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
            LOG("Calc discover_response sign: %s\r\n", sign_str);  

            cJSON_AddStringToObject(resp, "sign", sign_str);

            char *json_reply = cJSON_PrintUnformatted(resp);
            if(json_reply != NULL)
            {
                LOG("UDP Send JSON:\r\n%s\r\n", json_reply);
                uint16_t json_len = strlen(json_reply);
                uint16_t send_len = json_len + 1;// JSON + \n 总长度
                // 按总发送长度分配pbuf
                struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, send_len, PBUF_POOL);
                if(p_tx != NULL)
                {
                    memcpy(p_tx->payload, json_reply, json_len);
                    ((uint8_t *)p_tx->payload)[json_len] = '\n';// 补换行
                    err_t ret = udp_sendto(upcb, p_tx, IP_ADDR_BROADCAST, UDP_LISTEN_PORT);
                    LOG("UDP send status:%d\r\n", ret);
                    pbuf_free(p_tx);
                }
                free(json_reply);
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
            LOG("KEY value verification failed!\r\n");
            goto udp_task_err;
        }

        char local_mac[32] = {0};
        snprintf(local_mac, sizeof(local_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
                gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);

        if(strcmp(target_mac->valuestring, local_mac) != 0)
        {
            LOG("MAC address verification failed!\r\n");
            goto udp_task_err;
        }

        if(strcmp(target_sn->valuestring, DEVICE_SN) != 0)
        {
            LOG("SN verification failed!\r\n");
            goto udp_task_err;
        }

        uint64_t now_ts = Get_Unix_Second();
        int64_t time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
         if(time_diff > TIME_VALID_SEC)
        {
            LOG("Time verification failed!\r\n");
            // goto udp_task_err;
        }

        uint32_t curr_seq = (uint32_t)seq->valuedouble;
        if(curr_seq == g_last_cfg_seq)
        {
            LOG("Sequence number verification failed!\r\n");
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


        //LOG("UDP Sorted config_set JSON:\r\n%s\r\n", sign_buf_req);

        uint8_t calc_hash[32] = {0};
        hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_req, strlen(sign_buf_req));
        hmac_sha256_final(&hmac, calc_hash);

        char calc_sign[9] = {0};
        snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X",
                calc_hash[0], calc_hash[1], 
                calc_hash[2], calc_hash[3]);

        //LOG("UDP Calc config_set sign:%s\r\n", calc_sign);

        if(strcmp(sign->valuestring, calc_sign) != 0)
        {
            LOG("sign check failed!\r\nExpected: %s, Actual: %s\r\n", calc_sign, sign->valuestring);
            goto udp_task_err;
        }

        ip4_addr_t ip_addr, mask_addr, gw_addr, srv_addr;
        ipaddr_aton(ip->valuestring, &ip_addr);
        ipaddr_aton(netmask->valuestring, &mask_addr);
        ipaddr_aton(gateway->valuestring, &gw_addr);
        ipaddr_aton(host_ip->valuestring, &srv_addr);

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
        gnetif.flags |= NETIF_FLAG_BROADCAST;// 重启后再次确保广播权限

        LOG("Distribution network successfully and Flash saved\r\n");
        print_local_ip();
        osDelay(300); // 短暂延时等待网络/IP稳定

        if(sntp_inited == 0)
        {
            Lwip_SNTP_Init();
            sntp_inited = 1;
        }
        else//已经开过sntp，IP变了重启一次
        {
            sntp_stop();
            Lwip_SNTP_Init();
        }
        udp_send_device_online(upcb,2);//软件重启
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
    uint16_t copy_len = p->len < sizeof(msg.data)-1 ? p->len : (sizeof(msg.data)-1);
    memcpy(msg.data, p->payload, copy_len);
    msg.data[copy_len] = 0;
    msg.len = copy_len;
    msg.src_ip = *addr;
    msg.src_port = port;

    xQueueSend(udp_msg_queue, &msg, 0);
    pbuf_free(p);
}

//=====================================================================
// TCP 客户端任务（配网成功才连接，连接后自动发心跳）
void tcp_client_task(void *arg)
{

    static uint32_t stable_tick = 0;
    static uint32_t reconnect_tick = 0;
    const uint32_t stable_delay = 2000; // IP生效后延时2秒再连接
    const uint32_t reconnect_interval = 5000; // 失败后5秒才能再次重连
    // 上电初始化基准时间
    last_hb_tick = HAL_GetTick();
    while(1)
    {
        if(g_network_configured == 0)// 网络未配置：清空状态、断开TCP、复位所有计时
        {
            stable_tick = 0;
            reconnect_tick = 0;
            g_tcp_connected = 0;
            last_hb_tick = HAL_GetTick();
            hb_lost_cnt = 0;
            if(tcp_pcb != NULL)
            {
                tcp_abort(tcp_pcb);
                tcp_pcb = NULL;
            }
            osDelay(500);
            continue;
        }

        // 已连接：定时发心跳，纯单向发送，不等待回复
        if(tcp_pcb != NULL && g_tcp_connected == 1 && g_network_configured == 1)
        {
            uint32_t now = HAL_GetTick();
            // 定时发心跳（上位机不回也发）
            if(now - last_hb_tick >= HEARTBEAT_PERIOD_MS)
            {
                last_hb_tick = now;  // 更新计时基准
                // 连续HB_LOST_MAX次心跳无任何回复 → 断开TCP，触发重连
                if(hb_lost_cnt >= HB_LOST_MAX)
                {
                    LOG("Heartbeat timeout 3 times, restart TCP network!\r\n");
                    if(tcp_pcb != NULL)
                    {
                        tcp_abort(tcp_pcb);
                        tcp_pcb = NULL;
                    }
                    g_tcp_connected = 0;
                    hb_lost_cnt = 0;  // 计数清零，准备下次重连后重新统计
                    // 清空定时器消息队列，清除残留待发数据
                    xQueueReset(xTimerReqQueue);
                }
                else
                {
                    // 未超时，正常发送心跳
                    hb_lost_cnt++;
                    LOG("Heartbeat lost count: %d/%d\r\n", hb_lost_cnt, HB_LOST_MAX);
                    TaskInfo_t hb_msg = {0};
                    hb_msg.inj_id = 0;
                    xQueueSend(xTimerReqQueue, &hb_msg, 100U);
                }
            }
        }
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
               (now - reconnect_tick < reconnect_interval))
            {
                osDelay(100);
                continue;
            }
            if(tcp_pcb != NULL)
            {
                tcp_abort(tcp_pcb);
                tcp_pcb = NULL;
            }
            struct tcp_pcb *new_pcb = tcp_new();//临时变量创建，失败不污染全局
            if(new_pcb != NULL)
            {
                // 绑定错误回调
                new_pcb->errf = tcp_error_callback;
                LOG("Start connect server IP:%d.%d.%d.%d PORT:%d ......\r\n",
                    ip4_addr1(&g_server_ip),ip4_addr2(&g_server_ip),
                    ip4_addr3(&g_server_ip),ip4_addr4(&g_server_ip),
                    TCP_SERVER_PORT);
                    
                err_t ret = tcp_connect(new_pcb, &g_server_ip, TCP_SERVER_PORT, tcp_connected_cb);
                if(ret != ERR_OK)
                {
                    tcp_abort(new_pcb);
                    new_pcb = NULL;
                    g_tcp_connected = 0;
                    stable_tick = 0;
                    reconnect_tick = HAL_GetTick();
                    osDelay(1500);
                }                
                else
                {
                    tcp_pcb = new_pcb;//连接请求成功再赋值全局
                }
            }
            else // tcp_new 分配失败，直接重置重连计时，等待内存回收
            {
                LOG("tcp_new() failed, out of memory!\r\n");
                reconnect_tick = HAL_GetTick();
                osDelay(2000); // 强制延时2秒，给内存池回收时间
            }
        }
        osDelay(500);//增加到1s
    }
}

// TCP 错误回调（被动断连或连接建立失败都会调用，err参数区分原因）
static void tcp_error_callback(void *arg, err_t err)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *)arg;
    if(pcb != NULL)
    {
        tcp_abort(pcb);
    }
    tcp_pcb = NULL;
    g_tcp_connected = 0;// 链路异常，清空连接标记
    last_hb_tick = HAL_GetTick(); // 链路异常，重置心跳计时基准
    hb_lost_cnt = 0;   // 网络异常，清零心跳计数
    reconnect_tick = HAL_GetTick();
    LOG("TCP link error, code:%d\r\n", err);
    // if(err == ERR_MEM)// 针对内存不足错误，强制延时，避免疯狂重试
    // {
    //     osDelay(2000);
    // }
}

// TCP 连接成功回调
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if(err != ERR_OK)
    {
        tcp_abort(tpcb);
        tcp_pcb = NULL;
        g_tcp_connected = 0;  // 连接失败，清空标记
        last_hb_tick = HAL_GetTick(); // 连接失败，重置心跳计时基准
        LOG("TCP handshake fail\r\n");
        return err;
    }
    // tcp_err(tpcb, NULL);
    LOG("TCP connect success!\r\n");
    g_tcp_connected = 1;  // 三次握手成功，标记已连接
    last_hb_tick = HAL_GetTick(); // 重置心跳计时基准
    tcp_recv(tpcb, tcp_recv_cb);
    tcp_send_register();
    return ERR_OK;
}

// TCP 接收回调
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if(err != ERR_OK || p == NULL)
    {
        tcp_close(tpcb);
        tcp_pcb = NULL;
        g_tcp_connected = 0;  // 被动断连，清空标记
        last_hb_tick = HAL_GetTick(); // 被动断连，重置心跳计时基准
        hb_lost_cnt = 0;   // 被动断连，清零
        return ERR_CLSD;
    }

    memset(tcp_rx_buf, 0, RX_BUF_SIZE);
    uint16_t recv_len = p->len < (RX_BUF_SIZE - 1) ? p->len     : (RX_BUF_SIZE - 1);
    memcpy(tcp_rx_buf, p->payload, recv_len);

    tcp_parse_cmd(tpcb, tcp_rx_buf);
    tcp_recved(tpcb, p->len);
    pbuf_free(p);
    return ERR_OK;
}

// TCP 发送字符串
static void tcp_send_str(struct tcp_pcb *tpcb, const char *str)
{
    // 新增保护：PCB空 / 链路断开直接退出，禁止任何发送
    if(tpcb == NULL || g_tcp_connected != 1 || str == NULL || strlen(str)==0)
    {
        return;
    }
    size_t json_len = strlen(str);
    if(json_len == 0)
    {
        return;
    }

    // 总长度 = 原字符串 + 换行符
    u16_t send_len = (u16_t)(json_len + 1);
    u16_t free_buf = tcp_sndbuf(tpcb);
    //LOG("TCP send buffer size: %d, free: %d", tcp_sndbuf(tpcb), tcp_sndbuf(tpcb));
    if(free_buf < send_len)//剩余发送空间小于报文长度，直接丢包
    {
        LOG("tcp send buf full,drop pkt\r\n");
        return;
    }
    // 临时拼接 JSON + \n
    char send_buf[2048] = {0};
    if (json_len < sizeof(send_buf) - 1)
    {
        memcpy(send_buf, str, json_len);
        send_buf[json_len] = '\n';  // 追加换行符
    }
    else
    {
        LOG("tcp msg too long!\r\n");
        return;
    }
    err_t ret;
    // 第一步：先发送原始JSON内容
    ret = tcp_write(tpcb, str, json_len, TCP_WRITE_FLAG_COPY);
    if(ret != ERR_OK)
    {
        LOG("tcp_write json fail,err:%d",ret);
        goto tcp_send_err;
    }
    // 第二步：单独追加换行符 '\n'
    const char newline = '\n';
    ret = tcp_write(tpcb, &newline, 1, TCP_WRITE_FLAG_COPY);
    if(ret != ERR_OK)
    {
        LOG("tcp_write newline fail,err:%d",ret);
        goto tcp_send_err;
    }

    tcp_output(tpcb);
    return;

tcp_send_err:
    tcp_abort(tpcb);
    tcp_pcb = NULL;
    g_tcp_connected = 0;
    last_hb_tick = HAL_GetTick(); // 重置心跳计时基准
}


/**
 * @brief 填充心跳 heartbeat 专用数据
 * @param hb_param 心跳结构体指针
 */
void FillHeartBeatData(HeartBeatParam_t *hb_param)
{
    if(hb_param == NULL)
    {
        return;
    }
    memset(hb_param, 0, sizeof(HeartBeatParam_t));

    // 整机运行状态
    hb_param->run_status = main_sys_status.is_Busy;

    // 设备基础状态
    hb_param->dev_status.is_oiling = main_sys_status.is_Busy;
    if(main_sys_status.running_mode==SYS_MODE_STARTING)//如果是启动阶段，状态上报也显示为启动中但未注油
    {
        hb_param->run_status = 1;//启动中
        hb_param->dev_status.is_oiling = 0;//未注油
    }
    //当前运行的注油口ID0=无任务运行
    hb_param->dev_status.current_inj_id = g_current_running_inj_id;
     // 匹配判断：TCP绑定出油口 == 当前运行出油口，才使用TCP任务ID
    if(g_tcp_task_bind.bind_inj_id != 0 && g_current_running_inj_id != 0 && g_tcp_task_bind.bind_inj_id == g_current_running_inj_id)
    {
        strncpy(hb_param->dev_status.current_task_id, g_tcp_task_bind.task_id, sizeof(hb_param->dev_status.current_task_id) - 1);
    }
    else
    {
        // 不匹配/无运行任务/无TCP任务，填充本地任务标识
        strncpy(hb_param->dev_status.current_task_id, "0", sizeof(hb_param->dev_status.current_task_id) - 1);
    }
    hb_param->dev_status.current_task_id[sizeof(hb_param->dev_status.current_task_id) - 1] = '\0';
    hb_param->dev_status.current_task_start = 0;
    hb_param->dev_status.estimated_complete = 0;
}

// 按协议发送心跳报文
static void tcp_send_heartbeat(HeartBeatParam_t *p_param)
{
    if(p_param == NULL || tcp_pcb == NULL || g_tcp_connected !=1)
    {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Get_Unix_Second();

    // 签名用临时对象
    cJSON *temp_root = cJSON_CreateObject();//cJSON 构造一个与最终报文结构完全相同的临时对象（不含 sign）
    cJSON_AddStringToObject(temp_root, "device_id", DEVICE_ID);
    
    //device_status子节点
    cJSON *dev_st = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev_st, "current_inj_id", p_param->dev_status.current_inj_id);
    cJSON_AddStringToObject(dev_st, "current_task_id", p_param->dev_status.current_task_id);
    cJSON_AddNumberToObject(dev_st, "current_task_start", p_param->dev_status.current_task_start);
     cJSON_AddBoolToObject(dev_st, "is_oiling", p_param->dev_status.is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(temp_root, "device_status", dev_st);
    
    cJSON_AddNumberToObject(temp_root, "run_status", p_param->run_status);
    cJSON_AddNumberToObject(temp_root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(temp_root, "ts", ts);
    cJSON_AddStringToObject(temp_root, "type", "heartbeat");

    // 生成无sign的JSON字符串
    char *json_without_sign = cJSON_PrintUnformatted(temp_root);
    if (!json_without_sign) {
        cJSON_Delete(temp_root);
        cJSON_Delete(root);
        return; 
    }
    //LOG("TCP Sorted heartbeat JSON:\r\n%s\r\n", json_without_sign);
    uint8_t hash[32];
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)json_without_sign, strlen(json_without_sign));
    hmac_sha256_final(&hmac, hash);
    
    char sign_str[9];
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X", 
            hash[0], hash[1], hash[2], hash[3]);
            
    // 最终发送报文：和签名对象顺序完全一致，最后追加 sign
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
    
    cJSON *dev_st_send = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev_st_send, "current_inj_id", p_param->dev_status.current_inj_id);
    cJSON_AddStringToObject(dev_st_send, "current_task_id", p_param->dev_status.current_task_id);
    cJSON_AddNumberToObject(dev_st_send, "current_task_start", p_param->dev_status.current_task_start);
    cJSON_AddBoolToObject(dev_st_send, "is_oiling", p_param->dev_status.is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(root, "device_status", dev_st_send);
    
    cJSON_AddNumberToObject(root, "run_status", p_param->run_status);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "sign", sign_str);
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        tcp_send_str(tcp_pcb, str);
        LOG("TCP Send JSON:\r\n%s\r\n", str);
        free(str);
        g_tcp_seq++;
    }
    free(json_without_sign);
    cJSON_Delete(temp_root);//自动递归释放 cap、inj_arr、内部所有子对象
    cJSON_Delete(root);//自动递归释放 cap、inj_arr、内部所有子对象
}

/**
 * @brief 填充状态应答 state_response 专用数据
 * @param resp_param 状态应答结构体指针
 * @param port_idx 注油器下标(0 ~ INJECTOR_CNT-1)
 */
// void FillStateRespData(StateRespParam_t *resp_param, uint16_t port_idx)
// {
//     if(resp_param == NULL || port_idx >= INJECTOR_CNT)
//     {
//         return;
//     }
//     memset(resp_param, 0, sizeof(StateRespParam_t));

//     // 整机运行状态
//     resp_param->run_status = main_sys_status.is_Busy;

//     // 设备基础状态
//     resp_param->dev_status.is_oiling = main_sys_status.is_Busy;
//     if(main_sys_status.running_mode==SYS_MODE_STARTING)//如果是启动阶段，状态上报也显示为启动中但未注油
//     {
//         resp_param->run_status = 1;//启动中
//         resp_param->dev_status.is_oiling = 0;//未注油
//     }
//     resp_param->dev_status.current_inj_id = g_current_running_inj_id;

//     // 匹配判断：TCP绑定出油口 == 当前整机运行出油口，才使用TCP任务ID
//     if(g_tcp_task_bind.bind_inj_id != 0 && g_current_running_inj_id != 0 && g_tcp_task_bind.bind_inj_id == g_current_running_inj_id)
//     {
//         strncpy(resp_param->dev_status.current_task_id, g_tcp_task_bind.task_id, sizeof(resp_param->dev_status.current_task_id) - 1);
//     }
//     else
//     {
//         // 不匹配/无TCP任务固定填"0"
//         strncpy(resp_param->dev_status.current_task_id, "0", sizeof(resp_param->dev_status.current_task_id) - 1);
//     }
//     // 强制末尾补结束符兜底，防止无'\0'乱码
//     resp_param->dev_status.current_task_id[sizeof(resp_param->dev_status.current_task_id) - 1] = '\0';
    
//     resp_param->dev_status.current_task_start = 0;
//     resp_param->dev_status.estimated_complete = 0;

//     // 填充指定单个注油器参数
//     _INJECTOR_INFO *p_inj = &main_sys_status.injector[port_idx];
//     InjectorItem_t *p_item = &resp_param->injector;

//     p_item->inj_id = p_inj->injector_id;

//     if(p_inj->injectRequest == 98)
//     {
//         p_item->status = 2;
//         p_item->last_error_code = 98;
//         strcpy(p_item->last_error_msg, "inject run fail");
//     }
//     else if(p_inj->injectRequest == 66 || p_inj->injectRequest == 99)
//     {
//         p_item->status = 0;
//         p_item->last_error_code = 0;
//         strcpy(p_item->last_error_msg, "");
//     }
//     else
//     {
//         p_item->status = 1;
//         p_item->last_error_code = 0;
//         strcpy(p_item->last_error_msg, "");
//     }

//     // 单位转换：秒 → 毫秒
//     p_item->inj_ts = (uint32_t)p_inj->interval * 1000U;
//     p_item->inj_vs = p_inj->volume;
// }

static void FillSingleInjectorItem(InjectorItem_t *p_item, uint16_t port_idx)
{
    if(p_item == NULL || port_idx >= INJECTOR_CNT)
        return;
    // port_idx 0~7 对应 main_sys_status.injector[0] ~ [7]
    _INJECTOR_INFO *p_inj = &main_sys_status.injector[port_idx];
    _INJECTOR_ERR_INFO *p_err = &injector_err_list[port_idx];
    // 上报给上位机的inj_id还原为1~8
    p_item->inj_id = p_inj->injector_id;

    if(p_inj->injectRequest == 98)
    {
        p_item->status = 2;
    }
    else if(p_inj->injectRequest == 66 || p_inj->injectRequest == 99)
    {
        p_item->status = 0;
    }
    else
    {
        p_item->status = 1;
    }
    p_item->inj_ts = (uint32_t)p_inj->interval;
    p_item->inj_vs = p_inj->volume;

    // 填充全局故障数组的错误码、描述
    p_item->last_error_code = p_err->err_code;
    strncpy(p_item->last_error_msg, p_err->err_msg, sizeof(p_item->last_error_msg)-1);
    p_item->last_error_msg[sizeof(p_item->last_error_msg)-1] = '\0';
}

/**
 * @brief 填充整机公共状态（current_inj_id、run_status等）
 * @param resp_param 输出结构体
 */
static void FillCommonDeviceStatus(StateRespParam_t *resp_param)
{
    memset(resp_param, 0, sizeof(StateRespParam_t));
    // 整机运行状态
    resp_param->run_status = main_sys_status.is_Busy;
    resp_param->dev_status.is_oiling = main_sys_status.is_Busy;

    if(main_sys_status.running_mode==SYS_MODE_STARTING)
    {
        resp_param->run_status = 1;
        resp_param->dev_status.is_oiling = 0;
    }
    resp_param->dev_status.current_inj_id = g_current_running_inj_id;

    // task_id匹配逻辑不变
    if(g_tcp_task_bind.bind_inj_id != 0 && g_current_running_inj_id != 0 && g_tcp_task_bind.bind_inj_id == g_current_running_inj_id)
    {
        strncpy(resp_param->dev_status.current_task_id, g_tcp_task_bind.task_id, sizeof(resp_param->dev_status.current_task_id) - 1);
    }
    else
    {
        strncpy(resp_param->dev_status.current_task_id, "0", sizeof(resp_param->dev_status.current_task_id) - 1);
    }
    resp_param->dev_status.current_task_id[sizeof(resp_param->dev_status.current_task_id) - 1] = '\0';
    resp_param->dev_status.current_task_start = 0;
    resp_param->dev_status.estimated_complete = 0;
}

/**
 * @brief 填充单端口状态应答（原有逻辑兼容，用于指定端口查询）
 * @param resp_param 应答结构体
 * @param port_idx 端口下标
 */
void FillStateRespData(StateRespParam_t *resp_param, uint16_t port_idx)
{
    // port_idx 已经是转换后的数组下标 0~7
    if(resp_param == NULL || port_idx >= INJECTOR_CNT)
        return;
    FillCommonDeviceStatus(resp_param);
    FillSingleInjectorItem(&resp_param->injector, port_idx);
}



//状态请求上报
void tcp_send_state_response(StateRespParam_t *p_param)
{
    if(p_param == NULL || tcp_pcb == NULL || g_tcp_connected !=1)
    {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *inj_arr = cJSON_CreateArray();
    uint64_t ts = Get_Unix_Second();

    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "inj_id",          p_param->injector.inj_id);
    cJSON_AddNumberToObject(item, "inj_ts",          p_param->injector.inj_ts);
    cJSON_AddNumberToObject(item, "inj_vs",          p_param->injector.inj_vs);
    cJSON_AddNumberToObject(item, "last_error_code", p_param->injector.last_error_code);
    cJSON_AddStringToObject(item, "last_error_msg",  p_param->injector.last_error_msg);
    cJSON_AddNumberToObject(item, "status",          p_param->injector.status);
    cJSON_AddItemToArray(inj_arr, item);

    // 签名临时对象，字段顺序同heartbeat，仅type="state_response"
    cJSON *temp_root = cJSON_CreateObject();
    cJSON *inj_copy = cJSON_Duplicate(inj_arr, 1);

    cJSON_AddStringToObject(temp_root, "device_id", DEVICE_ID);
    // device_status 子对象：字典序
    cJSON *dev_st = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev_st, "current_inj_id",    p_param->dev_status.current_inj_id);
    cJSON_AddStringToObject(dev_st, "current_task_id",  p_param->dev_status.current_task_id);
    cJSON_AddNumberToObject(dev_st, "current_task_start",p_param->dev_status.current_task_start);
    cJSON_AddNumberToObject(dev_st, "estimated_complete",p_param->dev_status.estimated_complete);
    cJSON_AddBoolToObject(dev_st, "is_oiling", p_param->dev_status.is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(temp_root, "device_status", dev_st);

    cJSON_AddItemToObject(temp_root, "injectors", inj_copy);
    cJSON_AddNumberToObject(temp_root, "run_status", p_param->run_status);
    cJSON_AddNumberToObject(temp_root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(temp_root, "ts", ts);
    cJSON_AddStringToObject(temp_root, "type", "state_response"); //只改此处type

    char *json_without_sign = cJSON_PrintUnformatted(temp_root);
    if (!json_without_sign) {
        cJSON_Delete(temp_root);
        cJSON_Delete(inj_arr);
        cJSON_Delete(root);
        return;
    }
    //LOG("TCP Sorted state_response JSON:\r\n%s\r\n", json_without_sign);

    // HMAC签名
    uint8_t hash[32];
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)json_without_sign, strlen(json_without_sign));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9];
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
            hash[0], hash[1], hash[2], hash[3]);

    // 最终报文组装
    cJSON_AddStringToObject(root, "type", "state_response"); //应答报文type
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);

    cJSON *dev_st_send = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev_st_send, "current_inj_id", p_param->dev_status.current_inj_id);
    cJSON_AddStringToObject(dev_st_send, "current_task_id", p_param->dev_status.current_task_id);
    cJSON_AddNumberToObject(dev_st_send, "current_task_start", p_param->dev_status.current_task_start);
    cJSON_AddNumberToObject(dev_st_send, "estimated_complete", p_param->dev_status.estimated_complete);
    cJSON_AddBoolToObject(dev_st_send, "is_oiling", p_param->dev_status.is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(root, "device_status", dev_st_send);

    cJSON_AddItemToObject(root, "injectors", inj_arr);
    cJSON_AddNumberToObject(root, "run_status", p_param->run_status);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "sign", sign_str);
    
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG("TCP Send state_response JSON:\r\n%s\r\n", str);
        tcp_send_str(tcp_pcb, str);
        free(str);
        g_tcp_seq++;
    }

    free(json_without_sign);
    cJSON_Delete(temp_root);
    cJSON_Delete(root);
}

// 全量上报所有注油口状态（req_inj_id=0时调用）
void tcp_send_all_state_response(void)
{
    if(tcp_pcb == NULL || g_tcp_connected !=1)
        return;

    cJSON *root = cJSON_CreateObject();
    cJSON *inj_arr = cJSON_CreateArray();
    uint64_t ts = Get_Unix_Second();

    // 先填充整机公共状态
    StateRespParam_t temp_param;
    FillCommonDeviceStatus(&temp_param);

    // 循环填充全部出油口
    for(uint16_t i = 0; i < INJECTOR_CNT; i++)
    {
        InjectorItem_t item = {0};
        FillSingleInjectorItem(&item, i);

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "inj_id",          item.inj_id);
        cJSON_AddNumberToObject(obj, "inj_ts",          item.inj_ts);
        cJSON_AddNumberToObject(obj, "inj_vs",          item.inj_vs);
        cJSON_AddNumberToObject(obj, "last_error_code", item.last_error_code);
        cJSON_AddStringToObject(obj, "last_error_msg",  item.last_error_msg);
        cJSON_AddNumberToObject(obj, "status",          item.status);
        cJSON_AddItemToArray(inj_arr, obj);
    }

    // 签名临时对象
    cJSON *temp_root = cJSON_CreateObject();
    cJSON *inj_copy = cJSON_Duplicate(inj_arr, 1);

    cJSON_AddStringToObject(temp_root, "device_id", DEVICE_ID);
    cJSON *dev_st = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev_st, "current_inj_id",    temp_param.dev_status.current_inj_id);
    cJSON_AddStringToObject(dev_st, "current_task_id",  temp_param.dev_status.current_task_id);
    cJSON_AddNumberToObject(dev_st, "current_task_start",temp_param.dev_status.current_task_start);
    cJSON_AddNumberToObject(dev_st, "estimated_complete",temp_param.dev_status.estimated_complete);
    cJSON_AddBoolToObject(dev_st, "is_oiling", temp_param.dev_status.is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(temp_root, "device_status", dev_st);

    cJSON_AddItemToObject(temp_root, "injectors", inj_copy);
    cJSON_AddNumberToObject(temp_root, "run_status", temp_param.run_status);
    cJSON_AddNumberToObject(temp_root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(temp_root, "ts", ts);
    cJSON_AddStringToObject(temp_root, "type", "state_response");

    char *json_without_sign = cJSON_PrintUnformatted(temp_root);
    if (!json_without_sign) {
        cJSON_Delete(temp_root);
        cJSON_Delete(inj_arr);
        cJSON_Delete(root);
        return;
    }

    // HMAC签名
    uint8_t hash[32];
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)json_without_sign, strlen(json_without_sign));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9];
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
            hash[0], hash[1], hash[2], hash[3]);

    // 最终报文组装
    cJSON_AddStringToObject(root, "type", "state_response");
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);

    cJSON *dev_st_send = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev_st_send, "current_inj_id", temp_param.dev_status.current_inj_id);
    cJSON_AddStringToObject(dev_st_send, "current_task_id", temp_param.dev_status.current_task_id);
    cJSON_AddNumberToObject(dev_st_send, "current_task_start", temp_param.dev_status.current_task_start);
    cJSON_AddNumberToObject(dev_st_send, "estimated_complete", temp_param.dev_status.estimated_complete);
    cJSON_AddBoolToObject(dev_st_send, "is_oiling", temp_param.dev_status.is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(root, "device_status", dev_st_send);

    cJSON_AddItemToObject(root, "injectors", inj_arr);
    cJSON_AddNumberToObject(root, "run_status", temp_param.run_status);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "sign", sign_str);
    
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG("TCP Send all state_response JSON:\r\n%s\r\n", str);
        tcp_send_str(tcp_pcb, str);
        free(str);
        g_tcp_seq++;
    }

    free(json_without_sign);
    cJSON_Delete(temp_root);
    cJSON_Delete(root);
}

// 解析网关下发指令
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf)
{
    char *sign_src = NULL;    // 统一指向签名字符串
    cJSON *temp_sign = NULL;
    uint16_t req_inj_id = 0;  // 存储状态请求下发的端口ID
    char recv_stop_taskid[48] = {0};

    LOG("TCP Recv JSON:\r\n%s\r\n", buf);
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

    if(strcmp(device_id->valuestring, DEVICE_ID) != 0) goto exit;

    uint32_t curr_seq = (uint32_t)seq->valuedouble;
    uint64_t curr_ts  = (uint64_t)ts->valuedouble;

    uint64_t now_ts = Get_Unix_Second();
    int64_t time_diff = llabs((int64_t)now_ts - (int64_t)curr_ts);
    if(time_diff > TIME_VALID_SEC)  
    {
        LOG("Time verification failed!\r\n");
        // goto exit;
    }

    if(curr_seq == g_last_cmd_seq)
    {
        LOG("Sequence number verification failed!\r\n");
        // goto exit;
    }
    g_last_cmd_seq = curr_seq;

    char ts_cmd_str[21] = {0};
    uint64_to_str(curr_ts, ts_cmd_str);

    memset(g_task_order_buf, 0, sizeof(g_task_order_buf));
    g_task_order_cnt = 0;
    memset(tcp_sign_buf_2048, 0, sizeof(tcp_sign_buf_2048));

    if(strcmp(type->valuestring, "task_order") == 0)
    {
        cJSON *task_info_arr = cJSON_GetObjectItem(root, "task_info");
        if(!task_info_arr ||  task_info_arr->type != cJSON_Array) goto exit;

        int arr_size = cJSON_GetArraySize(task_info_arr);

        for(int i = 0; i < arr_size && i < TASK_DATA_MAX; i++)
        {
            cJSON *item = cJSON_GetArrayItem(task_info_arr, i);
            if(!item) continue;

            // 严格按英文字典序获取字段
            cJSON *p_inj_id  = cJSON_GetObjectItem(item, "inj_id");
            cJSON *p_inj_ts  = cJSON_GetObjectItem(item, "inj_ts");
            cJSON *p_inj_v   = cJSON_GetObjectItem(item, "inj_v");
            cJSON *p_task_id = cJSON_GetObjectItem(item, "task_id");
            
            // 字段必填合法性校验
            if(!p_task_id || !p_inj_id || !p_inj_v || !p_inj_ts)
            {
                continue;
            }

            TaskOrderInfo_t *p_buf = &g_task_order_buf[g_task_order_cnt];
            memset(p_buf,0,sizeof(TaskOrderInfo_t));
            strncpy(p_buf->task_id, p_task_id->valuestring, sizeof(p_buf->task_id) - 1);
            p_buf->inj_id     = (uint16_t)p_inj_id->valuedouble;
            p_buf->inj_ts     = (uint16_t)p_inj_ts->valuedouble;
            p_buf->inj_v      = (uint16_t)p_inj_v->valuedouble;

            g_task_order_cnt++;
        }
        //将任务逐条加入到队列
        for(int i = 0; i < g_task_order_cnt; i++)
        {
            TaskInfo_t item;
            memset(&item, 0, sizeof(TaskInfo_t)); // 先清空，防止脏数据
            item.inj_id = g_task_order_buf[i].inj_id;
            item.inj_ts = g_task_order_buf[i].inj_ts;
            item.inj_v  = g_task_order_buf[i].inj_v;
            //拷贝task_id，截断保护
            strncpy(item.task_id, g_task_order_buf[i].task_id, TASK_ID_LEN - 1);
            item.task_id[TASK_ID_LEN - 1] = '\0';
            // 入队，超时10ms
            xQueueSend(xTcpTaskQueue, &item, pdMS_TO_TICKS(10));
        }
        // 新建临时cJSON，按字典序构造签名原文
        temp_sign = cJSON_CreateObject();
        if(!temp_sign) goto exit;
        cJSON *arr_copy  = cJSON_Duplicate(task_info_arr, 1); // 深拷贝数组
        if(!arr_copy) goto exit;
        // 字典序：device_id → seq → task_info → ts → type
        cJSON_AddStringToObject(temp_sign, "device_id", DEVICE_ID);
        cJSON_AddNumberToObject(temp_sign, "seq", curr_seq);
        cJSON_AddItemToObject(temp_sign, "task_info", arr_copy);
        cJSON_AddNumberToObject(temp_sign, "ts", curr_ts);
        cJSON_AddStringToObject(temp_sign, "type", "task_order");

        // 导出签名字符串
        sign_src = cJSON_PrintUnformatted(temp_sign);
        if(sign_src)
        {
            //LOG("TCP Sorted task_order JSON:\r\n%s\r\n", sign_src);
        }
    }
    else if(strcmp(type->valuestring, "state_request") == 0)
    {
        cJSON *req_inj = cJSON_GetObjectItem(root, "inj_id");
        // 校验 inj_id 字段合法性
        if(!req_inj || req_inj->type != cJSON_Number)
        {
            LOG("state_request inj_id invalid!\r\n");
            goto exit;
        }
        req_inj_id = (uint16_t)req_inj->valuedouble;

        // 字典序：device_id → inj_id → seq → ts → type
        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"inj_id\":%u,\"seq\":%lu,\"ts\":%s,\"type\":\"state_request\"}",
            DEVICE_ID,
            req_inj_id,
            (unsigned long)curr_seq,
            ts_cmd_str
        );
        //LOG("TCP Sorted state_request JSON:\r\n%s\r\n", tcp_sign_buf_2048);
        sign_src = tcp_sign_buf_2048;
    }
    else if(strcmp(type->valuestring, "task_empty") == 0)
    {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        cJSON *wait = cJSON_GetObjectItem(root, "wait_time");
        if(!msg || msg->type != cJSON_String || !wait || wait->type != cJSON_Number)
        {
            goto exit;
        }
        // 字典序：device_id < message < seq < ts < type < wait_time
        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"message\":\"%s\",\"seq\":%lu,\"ts\":%s,\"type\":\"task_empty\",\"wait_time\":%d}",
            DEVICE_ID,
            msg->valuestring,
            (unsigned long)curr_seq,
            ts_cmd_str,
            (int)wait->valuedouble);

        //LOG("TCP Sorted task_empty JSON:\r\n%s\r\n", tcp_sign_buf_2048);
        sign_src = tcp_sign_buf_2048;
    }
    else if(strcmp(type->valuestring, "stop_task") == 0)
    {
        cJSON *task_id = cJSON_GetObjectItem(root, "task_id");
        if(!task_id || task_id->type != cJSON_String)
        {
            goto exit;
        }

        memset(g_stop_target_task_id, 0, sizeof(g_stop_target_task_id));
        strncpy(g_stop_target_task_id, task_id->valuestring, sizeof(g_stop_target_task_id)-1);
        g_stop_target_task_id[sizeof(g_stop_target_task_id)-1] = '\0';

        // 字典序：device_id → seq → task_id → ts → type
        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"seq\":%lu,\"task_id\":\"%s\",\"ts\":%s,\"type\":\"stop_task\"}",
            DEVICE_ID,
            (unsigned long)curr_seq,
            task_id->valuestring,
            ts_cmd_str);
        //LOG("TCP Sorted stop_task JSON:\r\n%s\r\n", tcp_sign_buf_2048);
        sign_src = tcp_sign_buf_2048;
    }
    else if(strcmp(type->valuestring, "heartbeat_ack") == 0)
    {
        // 提取新增字段并做合法性校验
        cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
        cJSON *has_task    = cJSON_GetObjectItem(root, "has_pending_task");
        if(!server_time || server_time->type != cJSON_Number || !has_task || (has_task->type != cJSON_True && has_task->type != cJSON_False))
        {
            goto exit;
        }

        // 此处读取服务端时间、待任务标记，存入全局变量
        uint64_t server_ts = (uint64_t)server_time->valuedouble;
        bool pending_task  = (has_task->type == cJSON_True) ? true : false;
        // 分别转换 ts、server_time 为字符串
        char server_ts_str[21] = {0};
        uint64_to_str_2(server_ts, server_ts_str, sizeof(server_ts_str));
        LOG("Server time: %s, Has pending task: %d\r\n", server_ts_str, pending_task);
        
        // 严格按英文字典序拼接签名字符串（剔除 sign 字段）
        // 字段顺序：device_id → has_pending_task → seq → server_time → ts → type
        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"has_pending_task\":%s,\"seq\":%lu,\"server_time\":%s,\"ts\":%s,\"type\":\"heartbeat_ack\"}",
            DEVICE_ID,
            has_task->type == cJSON_True ? "true" : "false",
            (unsigned long)curr_seq,
            server_ts_str,
            ts_cmd_str);

        //LOG("TCP Sorted heartbeat_ack JSON:\r\n%s\r\n", tcp_sign_buf_2048);
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

    //LOG("Calc TCP sign: %s\r\n", calc_sign);         
    if(strcmp(sign->valuestring, calc_sign) != 0)
    {
        LOG("sign check failed!\r\nExpected: %s, Actual: %s\r\n", calc_sign, sign->valuestring);
        goto exit;
    }
    LOG("TCP received message successfully!\r\n");

    //收到上位机任意合法报文，清零心跳丢失计数
    hb_lost_cnt = 0;

    // 判断当前指令类型，state_request 在此处回复应答
    if(strcmp(type->valuestring, "state_request") == 0)
    {
        if(req_inj_id == 0)
        {
            // 请求ID=0：上传全部INJECTOR_CNT个端口
            tcp_send_all_state_response();
        }
        else if(req_inj_id >= 1 && req_inj_id <= INJECTOR_CNT)
        {
            // 上位机ID 1~8 → 数组下标 = ID - 1
            uint16_t port_idx = req_inj_id - 1;
            StateRespParam_t resp_param;
            //填充指定端口数据
            FillStateRespData(&resp_param, port_idx);
            //发送状态响应报文
            tcp_send_state_response(&resp_param);
        }
        else
        {
            LOG("state_request inj_id out of range 1~%d\r\n", INJECTOR_CNT);
        }
    }
    if(strcmp(type->valuestring, "stop_task") == 0)
    {
        // 全局g_task_id非空，且下发task_id和当前运行任务一致
        if(strlen(g_tcp_task_bind.task_id) != 0 && strcmp(g_tcp_task_bind.task_id, g_stop_target_task_id) == 0)
        {
            g_force_stop_task = 1;
            strncpy(g_stop_target_task_id, recv_stop_taskid, sizeof(g_stop_target_task_id)-1);
            g_stop_target_task_id[sizeof(g_stop_target_task_id)-1] = '\0';
            LOG("Valid stop_task cmd matched task_id:%s, trigger emergency stop\r\n", recv_stop_taskid);
        }
        else
        {
            LOG("stop_task task_id mismatch, current tcp task:%s, req stop:%s\r\n", g_tcp_task_bind.task_id, recv_stop_taskid);
        }
    }

exit:
    if(temp_sign != NULL)
    {
        cJSON_Delete(temp_sign);
    }
    if(sign_src != tcp_sign_buf_2048 && sign_src != NULL)
    {
        free(sign_src);
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
        DEVICE_ID,
        DEVICE_FW_VER,
        mac_str,
        DEVICE_MODEL,
        (unsigned long)g_tcp_seq,
        ts_online_str);
    //LOG("TCP Sorted register JSON:\r\n%s\r\n", sign_buf_register);

    uint8_t hash[32] = {0};
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_register, strlen(sign_buf_register));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    cJSON_AddStringToObject(root, "type", "register");
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(root, "firmware_ver", DEVICE_FW_VER);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG("TCP Send JSON:\r\n%s\r\n", str);
        tcp_send_str(tcp_pcb, str);
        free(str);
        g_tcp_seq++;
    }
    cJSON_Delete(root);
}

/**
 * @brief  发送 task_request 任务请求报文
 * @param  info_arr: TaskInfo_t 数组首地址
 * @param  arr_len: 数组元素个数
 * @param  is_oiling: 是否正在注油
 */
void tcp_send_task_request(TaskInfo_t *info_arr, uint16_t arr_len, uint8_t is_oiling)
{
    if(tcp_pcb == NULL || g_tcp_connected !=1)
    {
        return;
    }
    if(info_arr == NULL || arr_len == 0)
    {
        LOG("task info array is empty!\r\n");
        return;
    }

    cJSON *root = cJSON_CreateObject();// 最终发送根节点
    cJSON *pending_arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < arr_len; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "inj_id", info_arr[i].inj_id);
        cJSON_AddItemToArray(pending_arr, item);
    }

    uint64_t ts = Get_Unix_Second();

    cJSON *temp_root = cJSON_CreateObject();// 2. 构造签名用的临时cJSON对象(不含 sign 字段)
     cJSON *pending_copy = cJSON_Duplicate(pending_arr, 1); // 深拷贝数组，隔离最终报文与签名报文
    
    cJSON_AddStringToObject(temp_root, "device_id", DEVICE_ID);
    
    // device_status子对象
    cJSON *dev_st_tmp = cJSON_CreateObject();
    cJSON_AddBoolToObject(dev_st_tmp, "is_oiling", is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(temp_root, "device_status", dev_st_tmp);
    
    cJSON_AddItemToObject(temp_root, "pending_requests", pending_copy);
    cJSON_AddNumberToObject(temp_root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(temp_root, "ts", ts); 
    cJSON_AddStringToObject(temp_root, "type", "task_request");

    char *json_no_sign = cJSON_PrintUnformatted(temp_root);
    if (json_no_sign == NULL)
    {
        cJSON_Delete(pending_arr);
        cJSON_Delete(temp_root);
        cJSON_Delete(root);
        return;
    }
    // LOG("TCP Sorted task_request JSON:\r\n%s\r\n", json_no_sign);

    // HMAC-SHA256 签名计算
    uint8_t hash[32] = {0};
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 组装最终发送报文（字段顺序无要求，ts 使用数字格式）
    cJSON_AddStringToObject(root, "type", "task_request");
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);

    cJSON *dev_st_send = cJSON_CreateObject();
    cJSON_AddBoolToObject(dev_st_send, "is_oiling", is_oiling ? cJSON_True : cJSON_False);
    cJSON_AddItemToObject(root, "device_status", dev_st_send);

    cJSON_AddItemToObject(root, "pending_requests", pending_arr);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "sign", sign_str);

    // 发送报文
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        tcp_send_str(tcp_pcb, str);
        LOG("TCP Send JSON:\r\n%s\r\n", str);
        free(str);
        g_tcp_seq++;
    }
    free(json_no_sign);
    cJSON_Delete(temp_root);
    cJSON_Delete(root);
}

// 执行结果上报
void tcp_send_execute_result(ExecuteResultInfo_t *info_arr, uint16_t arr_len)
{
    if(tcp_pcb == NULL || g_tcp_connected !=1)
    {
        return;
    }
    if(info_arr == NULL || arr_len == 0)
    {
        LOG("execute result array is empty!\r\n");
        return;
    }

    // 最终发送根节点
    cJSON *root = cJSON_CreateObject();
    // task_info 数组
    cJSON *task_info_arr = cJSON_CreateArray();

    // 填充数组子项
    for (uint16_t i = 0; i < arr_len; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "actual_volume",   info_arr[i].actual_volume);
        cJSON_AddNumberToObject(item, "execute_result", info_arr[i].execute_result);
        cJSON_AddNumberToObject(item, "inj_id",          info_arr[i].inj_id);
        cJSON_AddStringToObject(item, "task_id",         info_arr[i].task_id);
        cJSON_AddNumberToObject(item, "valve_act_tm",    info_arr[i].valve_act_tm);
        cJSON_AddNumberToObject(item, "valve_hum",       info_arr[i].valve_hum);
        cJSON_AddNumberToObject(item, "valve_temp",      info_arr[i].valve_temp);
        cJSON_AddNumberToObject(item, "valve_total_tm",  info_arr[i].valve_total_tm);
        // 仅 execute_result=3(失败) 时，追加错误字段（error_code / error_msg 字典序靠后）
        if(info_arr[i].execute_result == 3)
        {
            cJSON_AddNumberToObject(item, "error_code", info_arr[i].error_code);
            cJSON_AddStringToObject(item, "error_msg", info_arr[i].error_msg);
        }
        cJSON_AddItemToArray(task_info_arr, item);
    }

    uint64_t ts = Get_Unix_Second();

    // 构造签名用临时对象（不含 sign），字段严格字典序
    cJSON *temp_root = cJSON_CreateObject();
    cJSON *task_info_copy = cJSON_Duplicate(task_info_arr, 1);

    // 字典序：device_id → seq → task_info → ts → type
    cJSON_AddStringToObject(temp_root, "device_id", DEVICE_ID);
    cJSON_AddNumberToObject(temp_root, "seq", g_tcp_seq);
    cJSON_AddItemToObject(temp_root, "task_info", task_info_copy);
    cJSON_AddNumberToObject(temp_root, "ts", ts);
    cJSON_AddStringToObject(temp_root, "type", "execute_result");

    char *json_no_sign = cJSON_PrintUnformatted(temp_root);
    if (json_no_sign == NULL)
    {
        cJSON_Delete(task_info_arr);
        cJSON_Delete(temp_root);
        cJSON_Delete(root);
        return;
    }
    //LOG("TCP Sorted execute_result JSON:\r\n%s\r\n", json_no_sign);

    // HMAC-SHA256 计算签名
    uint8_t hash[32] = {0};
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)json_no_sign, strlen(json_no_sign));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // 组装最终发送报文（ts 为数字）
    cJSON_AddStringToObject(root, "type", "execute_result");
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
    cJSON_AddItemToObject(root, "task_info", task_info_arr);
    cJSON_AddNumberToObject(root, "seq", g_tcp_seq);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddStringToObject(root, "sign", sign_str);

    // 发送报文
    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        tcp_send_str(tcp_pcb, str);
        LOG("TCP Send JSON:\r\n%s\r\n", str);
        free(str);
        g_tcp_seq++;
    }

    // 统一释放
    free(json_no_sign);
    cJSON_Delete(temp_root);
    cJSON_Delete(root);
}

void TCP_task_processing_task(void *argument)
{
    TaskInfo_t recv_item={0};
    memset(&task_object_list[3], 0, sizeof(_TASK_OBJECT));//将任务列表3清零
    while(1)
    {
        if(xQueueReceive(xTcpTaskQueue, &recv_item, portMAX_DELAY) == pdPASS)
        {
            // 只有系统就绪模式才处理任务，其他模式直接丢弃
            if(main_sys_status.running_mode != SYS_MODE_READY)
            {
                LOG("System not ready mode, discard tcp task: inj= %d; vol= %d; inj_ts= %d; task_id= %s\r\n",
                    recv_item.inj_id, recv_item.inj_v, recv_item.inj_ts, recv_item.task_id);
                continue;
            }

            if(main_sys_status.is_Busy)
            {
                LOG("System busy, discard tcp task: inj= %d; vol= %d; inj_ts= %d; task_id= %s\r\n",
                    recv_item.inj_id, recv_item.inj_v, recv_item.inj_ts, recv_item.task_id);
                continue;
            }

            if(recv_item.inj_ts > 0)
            {
                uint8_t inj_id = recv_item.inj_id;
                // 校验出油口ID合法性 1~injector_count
                if(inj_id >= 1 && inj_id <= injector_count)
                {
                    uint8_t tag = inj_id - 1;
                    uint64_t new_sec = recv_item.inj_ts;
                    // 合法区间：5秒 ~ 40天
                    if(new_sec >= 5 && new_sec <= 3456000)
                    {
                        if(injector_config.injector[tag].interval != new_sec)
                        {
                            injector_config.injector[tag].interval = new_sec;
                            LOG("TCP update injector%d auto interval to %llu s", inj_id, new_sec);

                            save_injector_config(&injector_config);
                            sync_injector_timers();
                        }
                    }
                    else
                    {
                        logError("TCP invalid interval inj%d, ts_sec:%llu", inj_id, new_sec);
                    }
                }
                else
                {
                    LOG("TCP modify interval invalid inj_id:%d\r\n", inj_id);
                }
            }

            uint8_t idx = 3;//固定写入3号任务槽
            //if(task_object_list[idx].task_status != 1)
            if(task_object_list[idx].task_status == 0)
            {
                LOG("task_object_list[3].task_status %d\r\n",task_object_list[idx].task_status);
                task_object_list[idx].inject_id   = recv_item.inj_id;
                task_object_list[idx].val         = recv_item.inj_v;
                task_object_list[idx].task_status = 1;
                LOG("TCP task write slot3 ok: inj= %d; vol= %d; inj_ts= %d; task_id= %s\r\n",
                    recv_item.inj_id,recv_item.inj_v,recv_item.inj_ts,recv_item.task_id);
                // 仅任务成功落地时，更新全局任务ID
                memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
                strncpy(g_tcp_task_bind.task_id, recv_item.task_id, sizeof(g_tcp_task_bind.task_id)-1);
                g_tcp_task_bind.bind_inj_id = recv_item.inj_id;
            }
            else
            {
                // 槽位繁忙、任务丢弃，不更新 g_task_id
                LOG("slot3 busy,discard tcp task: inj= %d; vol= %d; inj_ts= %d; task_id= %s\r\n",
                    recv_item.inj_id,recv_item.inj_v,recv_item.inj_ts,recv_item.task_id);
            }
        }
    }
}

void timer_req_send_task(void *arg)
{
    g_task_request_cnt = 0;
    TaskInfo_t item;
    HeartBeatParam_t hb_data;
    for(;;)
    {
        if(xQueueReceive(xTimerReqQueue, &item, pdMS_TO_TICKS(50)) == pdPASS)
        {
            if(g_tcp_connected != 1 || tcp_pcb == NULL)// TCP未连接：丢弃本条上报，不调用发送函数
            {
                LOG("TCP offline, drop timer notify inj:%d\r\n", item.inj_id);
                continue;
            }
            if(item.inj_id == 0)// 心跳报文
            {
                memset(&hb_data, 0, sizeof(HeartBeatParam_t));
                FillHeartBeatData(&hb_data);
                tcp_send_heartbeat(&hb_data);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            // 注油任务：写入全局缓存，防溢出
            if(g_task_request_cnt < TASK_DATA_MAX)
            {
                g_task_request_buf[g_task_request_cnt] = item;
                g_task_request_cnt++;
                LOG("Cache inj task: %d, total:%d\r\n", item.inj_id, g_task_request_cnt);
            }
            else
            {
                LOG("Task buffer full, drop inj:%d\r\n", item.inj_id);
                continue;
            }
           // 聚合窗口：500ms 收集短时内所有后续任务
            while(xQueueReceive(xTimerReqQueue, &item, pdMS_TO_TICKS(500)) == pdPASS)
            {
                if(g_tcp_connected != 1 || tcp_pcb == NULL)
                {
                    LOG("TCP offline, drop inj:%d\r\n", item.inj_id);
                    break;
                }

                // 窗口内收到心跳，立即单独发送
                if(item.inj_id == 0)
                {
                    memset(&hb_data, 0, sizeof(HeartBeatParam_t));
                    FillHeartBeatData(&hb_data);
                    tcp_send_heartbeat(&hb_data);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                // 继续存入缓存
                if(g_task_request_cnt < TASK_DATA_MAX)
                {
                    g_task_request_buf[g_task_request_cnt] = item;
                    g_task_request_cnt++;
                    LOG("Cache inj task: %d, total:%d\r\n", item.inj_id, g_task_request_cnt);
                }
                else
                {
                    LOG("Task buffer full, drop inj:%d\r\n", item.inj_id);
                }
            }

            // 聚合结束，批量上报缓存里的所有任务
            if(g_task_request_cnt > 0)
            {
                uint8_t busy = main_sys_status.is_Busy;
                //uint8_t busy = 0;//测试阶段不区分系统忙闲，定时上报任务请求，确保上位机及时收到注油请求，提升响应速度
                if(task_object_list[3].task_status == 1)
                {
                    busy = 1;
                }
                // 批量发送：传数组首地址 + 有效数量
                //LOG("Batch send %d inj tasks\r\n", g_task_request_cnt);
                tcp_send_task_request(g_task_request_buf, g_task_request_cnt, busy);
                // 清空计数，准备下一轮聚合
                g_task_request_cnt = 0;
                //vTaskDelay(pdMS_TO_TICKS(200));
            }
         }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void TimeSyncTask(void *arg)
{
    char ntp_time_buf[32] = {0};
    char rtc_time_buf[32] = {0};
    char diff_buf[24] = {0};
    char ts_str[21]  = {0};
    char ts_buf[24] = {0};

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
                        LOG("Power-on first sync NTP -> RTC OK");
                    }
                    else
                    {
                        uint64_to_str_2(diff, diff_buf, sizeof(diff_buf));
                        LOG("Time diff: %s ms > threshold, re-sync", diff_buf);
                    }
                }
            }

            // 打印NTP时间戳和RTC时间戳，观察两者是否一致（误差在TIME_DIFF_THRESHOLD毫秒内，符合预期）
            if(sys_unix_ms > 0)
            {
                stamp_to_time(sys_unix_ms, ntp_time_buf, sizeof(ntp_time_buf));
                uint64_to_str_2(sys_unix_ms, ts_str, sizeof(ts_str));
                LOG("NTPTime: %s | MS_TS: %s", ntp_time_buf, ts_str);
            }
            else
            {
                LOG("NTP sync fail, ts=0");
            }
            //RTC时间打印，观察是否与NTP时间一致
            rtc_ms = RTC_To_UnixMs();
            stamp_to_time(rtc_ms, rtc_time_buf, sizeof(rtc_time_buf));
            uint64_to_str_2(rtc_ms, ts_buf, sizeof(ts_buf));
            LOG("RTCTime: %s | MS_TS: %s\r\n", rtc_time_buf, ts_buf);
            last_check_tick = now_tick;
        }
        osDelay(100); // 小延时，让出CPU
    }
}

void test_oil_filling_task(void *arg)
{
    uint16_t fill_total = 0;
    uint16_t fill_cur = 0;
    //ExecuteResultInfo_t exec_info;
    const uint8_t slot_idx = 3;
    uint32_t single_start_tick;   // 单次单mL起始时间戳
    uint32_t single_cost_ms;      // 单次实际耗时
    uint32_t total_cost_ms = 0;   // 累计总耗时
    uint8_t fill_err_flag = 0;    // 注油异常标记
    uint16_t fill_err_code = 0;
    char fill_err_str[32] = {0};

    for(;;)
    {
        osDelay(100); // 小延时，让出CPU
        // 只处理固定3号槽位
        if(task_object_list[slot_idx].task_status != 1U)
        {
            continue;
        }
        // 取出本次总油量
        fill_total = task_object_list[slot_idx].val;
        fill_cur = 0;
        total_cost_ms = 0;
        fill_err_flag = 0;
        main_sys_status.is_Busy = 1U; // 标记系统忙碌
        LOG("Start oil filling, inj_id:%d, target total vol:%d mL\r\n",
        task_object_list[slot_idx].inject_id, fill_total);

        // 逐毫升循环注油，并模拟每毫升注油耗时，同时上报当前执行结果
        while(fill_cur < fill_total)
        {
            // 检测上位机下发紧急停止指令
            if(g_force_stop_task == 1)
            {
                fill_err_flag = 1;
                fill_err_code = 200;  // 自定义紧急停止错误码
                strncpy(fill_err_str, "Emergency stop by remote cmd", sizeof(fill_err_str)-1);
                fill_err_str[sizeof(fill_err_str)-1] = '\0';
                LOG("Remote stop task triggered, break filling loop\r\n");
                break;
            }
            fill_cur++; // 1mL
            //记录当前mL开始时间戳
            single_start_tick = HAL_GetTick();
            //模拟单毫升注油2秒 
            osDelay(5000);
            // 计算本次真实耗时
            single_cost_ms = HAL_GetTick() - single_start_tick;
            total_cost_ms += single_cost_ms;

            // 【示例：可在这里增加真实故障判断逻辑】
            // if(检测到阀体故障)
            // {
            //     fill_err_flag = 1;
            //     fill_err_code = 101;
            //     strncpy(fill_err_str, "Valve drive fault", sizeof(fill_err_str)-1);
            //     break;
            // }

            //组装单条执行结果并上报
            memset(&exec_info, 0, sizeof(ExecuteResultInfo_t));
            // 基础任务信息
            //任务ID和注射器ID从全局任务槽获取，确保上报与当前执行任务一致
            strncpy(exec_info.task_id, g_tcp_task_bind.task_id, sizeof(exec_info.task_id)-1);
            exec_info.task_id[sizeof(exec_info.task_id)-1] = '\0';
            exec_info.inj_id = task_object_list[slot_idx].inject_id;

            // 执行状态：中间进度临时标记成功
            exec_info.execute_result = 1;//进行中
            exec_info.actual_volume = fill_cur;

            // 填入实测单次耗时、累计总耗时
            exec_info.valve_act_tm = (uint16_t)single_cost_ms;
            exec_info.valve_total_tm = (uint16_t)total_cost_ms;

            // 温湿度这里填固定模拟值，实际项目替换成传感器读取
            exec_info.valve_temp = 45;
            exec_info.valve_hum = 38;

            // 无错误
            exec_info.error_code = 0;
            strncpy(exec_info.error_msg, "OK", sizeof(exec_info.error_msg)-1);
            exec_info.error_msg[sizeof(exec_info.error_msg)-1] = '\0';

            // 单条进度上报
            tcp_send_execute_result(&exec_info, 1U);
            LOG("Filling progress: current %d mL / total %d mL\r\n", fill_cur, fill_total);
        }
        memset(&exec_info, 0, sizeof(ExecuteResultInfo_t));
        strncpy(exec_info.task_id, g_tcp_task_bind.task_id, sizeof(exec_info.task_id)-1);
        exec_info.task_id[sizeof(exec_info.task_id)-1] = '\0';
        exec_info.inj_id = task_object_list[slot_idx].inject_id;

        exec_info.actual_volume = fill_cur;      // 实际总出油量
        exec_info.valve_act_tm = (uint16_t)single_cost_ms;
        exec_info.valve_total_tm = (uint16_t)total_cost_ms;

        exec_info.valve_temp = 45;
        exec_info.valve_hum = 38;

        if(fill_err_flag == 0)
        {
            // 正常完成
            exec_info.execute_result = 2;
            exec_info.error_code = 0;
            strncpy(exec_info.error_msg, "Filling complete successfully", sizeof(exec_info.error_msg)-1);
        }
        else
        {
            // 执行失败 execute_result=3，携带错误码和描述
            exec_info.execute_result = 3;
            exec_info.error_code = fill_err_code;
            strncpy(exec_info.error_msg, fill_err_str, sizeof(exec_info.error_msg)-1);
        }
        exec_info.error_msg[sizeof(exec_info.error_msg)-1] = '\0';
        //结果上报
        tcp_send_execute_result(&exec_info, 1U);
        // 清空当前运行出油口标记
        g_current_running_inj_id = 0;
        //注油结束，清空全局任务ID
        memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
        //释放任务槽、取消系统忙碌标记
        task_object_list[slot_idx].task_status = 0U;
        main_sys_status.is_Busy = 0U;

        // 复位停止全局标志，允许下次新任务停止
        g_force_stop_task = 0;
        memset(g_stop_target_task_id, 0, sizeof(g_stop_target_task_id));

        if(fill_err_flag)
        {
            LOG("Oil filling FAIL, inj_id:%d, err_code:%d, msg:%s\r\n",
                task_object_list[slot_idx].inject_id, fill_err_code, fill_err_str);
        }
        else
        {
            LOG("Oil filling finished, inj_id:%d, actual total vol:%d mL, total run time:%d ms\r\n",
                task_object_list[slot_idx].inject_id, fill_total, total_cost_ms);
        }
    }
}

