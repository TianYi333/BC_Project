#include "net_comm_task.h"

//==== 全局静态缓冲区，替代函数内局部大数组 ====
static char sign_buf_req[512];   // 专门给 request 验签用
static char sign_buf_resp[512];  // 专门给 response 组包用
static char sign_buf_online[512];  // 专门给 online 组包用
static char tcp_sign_buf_1024[1024];
static char tcp_sign_buf_2048[2048];

static char tcp_rx_buf[RX_BUF_SIZE];
static struct tcp_pcb *tcp_pcb = NULL;
static uint32_t g_seq = 1;  // 全局自增序列号，上电从 1 开始
static uint32_t g_last_cfg_seq = 0;           // 上位机 -> 设备：UDP报文 seq
static uint32_t g_last_cmd_seq = 0;           // 上位机 -> 设备：指令报文 seq 

static uint8_t  g_network_configured = 0;         // 配网成功标志：0=未配网 1=已配网

uint8_t g_is_reboot = 1;// 设备上电状态标记：true=重启/非首次上电  false=首次上电(无历史配网)

static uint32_t reconnect_tick = 0;// TCP重连节流计时，避免频繁重连
static uint8_t  g_tcp_connected = 0;  // 0=未真正连接 1=握手成功


static ip4_addr_t  g_server_ip;                // 上位机业务服务器IP（动态保存）
// 网络配置结构体（固化到Flash）
static NetConfig_t g_net_cfg;
// 网络独立数据库句柄
struct fdb_kvdb net_kvdb;

HMAC_SHA256_CTX hmac;
static TimerHandle_t xHeartbeatTimer = NULL;
static uint8_t bSendHeartbeatFlag = 0;

static void udp_recv_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static void udp_send_device_online(struct udp_pcb *upcb);
static void udp_msg_process(const char *buf, const ip_addr_t *src_ip, u16_t src_port, struct udp_pcb *upcb);
static void tcp_error_callback(void *arg, err_t err);
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf);
static void tcp_send_str(struct tcp_pcb *tpcb, const char *str);
static void tcp_send_heartbeat(void);
static void tcp_send_device_online(void);
static void vHeartbeatTimerCallback(TimerHandle_t xTimer);


static void vHeartbeatTimerCallback(TimerHandle_t xTimer)
{
    bSendHeartbeatFlag = 1;
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

    result = fdb_kvdb_init(&net_kvdb, "netdb", "ef_kvdb1", &default_kv, NULL);

    if(result != FDB_NO_ERR) {
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

        autoip_stop(&gnetif);  // 关闭自动IP协商
        netif_set_down(&gnetif);
        netif_set_up(&gnetif);

        gnetif.flags |= NETIF_FLAG_BROADCAST;//网口启用后，强制开启广播权限

        g_server_ip = srv_ip;
        g_network_configured = 1;
        LOG("Flash loading network configuration succeeded, TCP started");
    }
    else
    {
        autoip_start(&gnetif);   // 强制启动本地链路IP
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

/**
 * @brief  UDP设备发现任务，端口9527（始终运行）
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
        osDelay(500); // 短暂延时保证网络稳定
        udp_send_device_online(upcb);
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
static void udp_send_device_online(struct udp_pcb *upcb)
{
    if(upcb == NULL) return;
    gnetif.flags |= NETIF_FLAG_BROADCAST;// 确保广播权限

    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Time_To_Unix();
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

    // 签名原文（保持原有字典序不变）
    snprintf(sign_buf_online, sizeof(sign_buf_online)-1,
            "{\"cur_ip\":\"%s\",\"firmware_ver\":\"%s\",\"is_reboot\":%s,\"mac\":\"%s\",\"model\":\"%s\",\"reason\":0,\"seq\":%lu,\"sn\":\"%s\",\"ts\":%s,\"type\":\"%s\"}",
            ip4addr_ntoa(netif_ip4_addr(&gnetif)),
            DEVICE_FW_VER,
            boot_str,
            mac_str,
            DEVICE_MODEL,
            (unsigned long)g_seq,
            DEVICE_SN,
            ts_online_str,
            "device_online");
    LOG("UDP Sorted device_online JSON:\r\n%s\r\n", sign_buf_online);
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
    cJSON_AddNumberToObject(root, "reason", 0);//0-上电，1-看门狗，2-软件重启（目前统一暂为0）
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "seq", g_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        LOG("UDP Send JSON:\r\n%s\r\n", str);
        uint16_t send_len = strlen(str);
        struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, send_len, PBUF_POOL);
        if(p_tx != NULL)
        {

            memcpy(p_tx->payload, str, send_len);
            // UDP 广播发送（对应上位机发现端口 UDP_LISTEN_PORT）
            err_t ret = udp_sendto(upcb, p_tx, IP_ADDR_BROADCAST, UDP_LISTEN_PORT);
            LOG("UDP device_online send status:%d\r\n", ret);
            pbuf_free(p_tx);
        }
        free(str);
        g_seq++;
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
        // uint64_t now_ts = Time_To_Unix();
        // int64_t  time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
        // if (time_diff > TIME_VALID_SEC)  goto udp_task_err;

        uint32_t curr_gw_seq = (uint32_t)seq->valuedouble;
        // if(curr_gw_seq <= g_last_cfg_seq ) goto udp_task_err;
        // g_last_cfg_seq = curr_gw_seq;


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

        LOG("UDP Sorted discover_request JSON:\r\n%s\r\n", sign_buf_req);

        uint8_t gw_calc_hash[32] = {0};
        hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_req, strlen(sign_buf_req));
        hmac_sha256_final(&hmac, gw_calc_hash);

        char gw_calc_sign[9] = {0};
        snprintf(gw_calc_sign, sizeof(gw_calc_sign), "%02X%02X%02X%02X",
                gw_calc_hash[0], gw_calc_hash[1],
                gw_calc_hash[2], gw_calc_hash[3]);
        LOG("Calc discover_request sign: %s\r\n", gw_calc_sign);      
        //签名不匹配 → 非法报文
        if (strcmp(sign->valuestring, gw_calc_sign) != 0) {
             LOG("Invalid sign, discard the packet\r\n");
             goto udp_task_err;
        }

        {
            cJSON *resp = cJSON_CreateObject();
            uint8_t *mac_addr = gnetif.hwaddr;
            char mac_str[32];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                    mac_addr[0], mac_addr[1], mac_addr[2],
                    mac_addr[3], mac_addr[4], mac_addr[5]);
            cJSON_AddStringToObject(resp, "mac", mac_str);
            cJSON_AddStringToObject(resp, "sn", "OIL-2026-0001");
            cJSON_AddStringToObject(resp, "model", "LUB-CTRL-V1.0");
            cJSON_AddStringToObject(resp, "firmware_ver", "1.0.3");
            cJSON_AddStringToObject(resp, "cur_ip", ip4addr_ntoa(netif_ip4_addr(&gnetif)));

            uint64_t real_ts = Time_To_Unix();
            // cJSON_AddNumberToObject(resp, "ts", real_ts);
            cJSON_AddNumberToObject(resp, "ts", ts_num+20);//调试阶段使用发送来的时间戳
            cJSON_AddNumberToObject(resp, "seq", g_seq);
            cJSON_AddStringToObject(resp, "type", "discover_response");

            memset(sign_buf_resp, 0, sizeof(sign_buf_resp));
            char ts_buf[21] = {0};
            // uint64_to_str(real_ts, ts_buf);
            uint64_to_str(ts_num+20, ts_buf);//调试阶段使用发送来的时间戳

            snprintf(sign_buf_resp, sizeof(sign_buf_resp)-1,
                "{\"cur_ip\":\"%s\",\"firmware_ver\":\"%s\",\"mac\":\"%s\",\"model\":\"%s\",\"seq\":%u,\"sn\":\"%s\",\"ts\":%s,\"type\":\"%s\"}",
                ip4addr_ntoa(netif_ip4_addr(&gnetif)),
                DEVICE_FW_VER,
                mac_str,
                DEVICE_MODEL,
                (unsigned int)g_seq,
                DEVICE_SN,
                ts_buf,
                "discover_response"
            );
            LOG("UDP Sorted discover_response JSON:\r\n%s\r\n", sign_buf_resp);

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
                uint16_t send_len = strlen(json_reply);
                struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, send_len, PBUF_POOL);
                if(p_tx != NULL)
                {
                    memcpy(p_tx->payload, json_reply, send_len);
                    err_t ret = udp_sendto(upcb, p_tx, IP_ADDR_BROADCAST, UDP_LISTEN_PORT);
                    LOG("UDP send status:%d\r\n", ret);
                    pbuf_free(p_tx);
                }
                free(json_reply);
                g_seq++;
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
        cJSON *host_ip     = cJSON_GetObjectItem(root, "host_ip"); // 新增服务器IP字段
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

        uint64_t now_ts = Time_To_Unix();
        int64_t time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
        // if(time_diff > TIME_VALID_SEC)
        // {
        //     LOG("Time verification failed!\r\n");
        //     goto udp_task_err;
        // }

        uint32_t curr_seq = (uint32_t)seq->valuedouble;
        // if(curr_seq <= g_last_cfg_seq)
        // {
        //     LOG("Sequence number verification failed!\r\n");
        //     goto udp_task_err;
        // }
        // g_last_cfg_seq = curr_seq;

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


        LOG("UDP Sorted config_set JSON:\r\n%s\r\n", sign_buf_req);

        uint8_t calc_hash[32] = {0};
        hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
        hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_req, strlen(sign_buf_req));
        hmac_sha256_final(&hmac, calc_hash);

        char calc_sign[9] = {0};
        snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X",
                calc_hash[0], calc_hash[1], 
                calc_hash[2], calc_hash[3]);

        LOG("UDP Calc config_set sign:\r\n%s\r\n", calc_sign);

        if(strcmp(sign->valuestring, calc_sign) != 0)
        {
            LOG("Invalid sign, discard the packet\r\n");
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
        udp_send_device_online(upcb);
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
    static uint8_t bTimerInit = 0;
    if(!bTimerInit)
    {
        bTimerInit = 1;
        xHeartbeatTimer = xTimerCreate("hbTimer", pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS), pdTRUE, 0, vHeartbeatTimerCallback);
        xTimerStart(xHeartbeatTimer, portMAX_DELAY);
        vTaskPrioritySet(xTimerGetTimerDaemonTaskHandle(), 6);
    }

    while(1)
    {
        if(g_network_configured == 0)
        {
            stable_tick = 0;
            reconnect_tick = 0;
            g_tcp_connected = 0;
            bSendHeartbeatFlag = 0;
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
            // 1. 定时发心跳（上位机不回也发）
            if(bSendHeartbeatFlag)
            {
                bSendHeartbeatFlag = 0;
                LOG("TCP Send heartbeat message!\r\n");
                tcp_send_heartbeat();
            }
        }
        if(tcp_pcb == NULL)// TCP自动重连
        {
            if(stable_tick == 0)
            {
                stable_tick = osKernelGetTickCount();
                reconnect_tick = osKernelGetTickCount();
            }
            // 等待网络参数稳定
            if((osKernelGetTickCount() - stable_tick < pdMS_TO_TICKS(stable_delay)) ||
               (osKernelGetTickCount() - reconnect_tick < pdMS_TO_TICKS(reconnect_interval)))
            {
                osDelay(100);
                continue;
            }
            tcp_pcb = tcp_new();
            LOG("TCP reconnection......\r\n");
            if(tcp_pcb != NULL)
            {
                // 绑定错误回调
                tcp_pcb->errf = tcp_error_callback;
                LOG("Start connect server IP:%d.%d.%d.%d PORT:%d\r\n",
                    ip4_addr1(&g_server_ip),ip4_addr2(&g_server_ip),
                    ip4_addr3(&g_server_ip),ip4_addr4(&g_server_ip),
                    TCP_SERVER_PORT);
                    
                err_t ret = tcp_connect(tcp_pcb, &g_server_ip, TCP_SERVER_PORT, tcp_connected_cb);
                if(ret != ERR_OK)
                {
                    tcp_pcb = NULL;
                    g_tcp_connected = 0;
                    stable_tick = 0;
                    reconnect_tick = osKernelGetTickCount();
                    osDelay(1500);
                }
            }
            else // tcp_new 分配失败，直接重置重连计时，等待内存回收
            {
                LOG("tcp_new() failed, out of memory!\r\n");
                tcp_pcb = NULL;
                reconnect_tick = osKernelGetTickCount();
                osDelay(2000); // 强制延时2秒，给内存池回收时间
            }
        }
        osDelay(500);
    }
}

// TCP 错误回调（被动断连或连接建立失败都会调用，err参数区分原因）
static void tcp_error_callback(void *arg, err_t err)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *)arg;
    if(pcb != NULL)
    {
        tcp_err(pcb, NULL); 
        tcp_abort(pcb);
    }
    tcp_pcb = NULL;
    g_tcp_connected = 0;// 链路异常，清空连接标记
    bSendHeartbeatFlag = 0;
    reconnect_tick = osKernelGetTickCount();
    LOG("TCP link error, code:%d\r\n", err);
    if(err == ERR_MEM)// 针对内存不足错误，强制延时，避免疯狂重试
    {
        osDelay(2000);
    }
}

// TCP 连接成功回调
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if(err != ERR_OK)
    {
        LOG("TCP handshake fail\r\n");
        tcp_pcb = NULL;
        g_tcp_connected = 0;  // 连接失败，清空标记
        return err;
    }
    // tcp_err(tpcb, NULL);
    LOG("TCP connect success!\r\n");
    g_tcp_connected = 1;  // 三次握手成功，标记已连接
    tcp_recv(tpcb, tcp_recv_cb);
    //tcp_send_device_online();
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
        bSendHeartbeatFlag = 0;
        return ERR_CLSD;
    }

    memset(tcp_rx_buf, 0, RX_BUF_SIZE);
    uint16_t recv_len = p->len < (RX_BUF_SIZE - 1) ? p->len : (RX_BUF_SIZE - 1);
    memcpy(tcp_rx_buf, p->payload, recv_len);

    tcp_parse_cmd(tpcb, tcp_rx_buf);
    tcp_recved(tpcb, p->len);
    pbuf_free(p);
    return ERR_OK;
}

// 按协议发送心跳报文
static void tcp_send_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cap = cJSON_CreateArray();
    uint64_t ts = Time_To_Unix();

    for(int i = 1; i <= 8; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "inj_id", i);
        cJSON_AddNumberToObject(item, "inj_jr", 6);
        cJSON_AddNumberToObject(item, "inj_st", 0);
        cJSON_AddNumberToObject(item, "inj_tr", 800);
        cJSON_AddNumberToObject(item, "inj_ts", 1200);
        cJSON_AddNumberToObject(item, "inj_vs", 10);
        cJSON_AddItemToArray(cap, item);
    }

    char ts_heartbeat_str[21] = {0};
    uint64_to_str(ts, ts_heartbeat_str);
    // 签名用临时对象
    cJSON *temp_root = cJSON_CreateObject();//cJSON 构造一个与最终报文结构完全相同的临时对象（不含 sign）
    cJSON *cap_copy = cJSON_Duplicate(cap, 1);// 深拷贝数组，避免直接使用 cap
    cJSON_AddItemToObject(temp_root, "capability", cap_copy);
    cJSON_AddStringToObject(temp_root, "device_id", DEVICE_ID);
    cJSON_AddNumberToObject(temp_root, "last_period_seq", 99);
    cJSON_AddNumberToObject(temp_root, "last_sub_index", 3);
    cJSON_AddStringToObject(temp_root, "last_task_id", "TASK_1746601000_003");
    cJSON_AddNumberToObject(temp_root, "last_task_result", 1);
    cJSON_AddNumberToObject(temp_root, "run_status", 0);
    cJSON_AddNumberToObject(temp_root, "seq", g_seq);
    cJSON_AddStringToObject(temp_root, "ts", ts_heartbeat_str); // 注意这里ts用字符串
    cJSON_AddStringToObject(temp_root, "type", "heartbeat");

    // 生成无sign的JSON字符串
    char *json_without_sign = cJSON_PrintUnformatted(temp_root);
    if (!json_without_sign) {
        cJSON_Delete(temp_root);
        cJSON_Delete(cap);
        cJSON_Delete(root);
        return;
    }
    LOG("TCP Sorted heartbeat JSON:\r\n%s\r\n", json_without_sign);
    uint8_t hash[32];
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)json_without_sign, strlen(json_without_sign));
    hmac_sha256_final(&hmac, hash);
    
    char sign_str[9];
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X", 
            hash[0], hash[1], hash[2], hash[3]);
    
    cJSON_AddItemToObject(root, "capability", cap);
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
    cJSON_AddNumberToObject(root, "last_period_seq", 99);
    cJSON_AddNumberToObject(root, "last_sub_index", 3);
    cJSON_AddStringToObject(root, "last_task_id", "TASK_1746601000_003");
    cJSON_AddNumberToObject(root, "last_task_result", 1);
    cJSON_AddNumberToObject(root, "run_status", 0);
    cJSON_AddNumberToObject(root, "seq", g_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);
    cJSON_AddNumberToObject(root, "ts", Time_To_Unix());
    cJSON_AddStringToObject(root, "type", "heartbeat");

    char *str = cJSON_PrintUnformatted(root);

    if(str)
    {
        LOG("TCP Send JSON:\r\n%s\r\n", str);
        tcp_send_str(tcp_pcb, str);
        free(str);
        g_seq++;
    }
    free(json_without_sign);
    cJSON_Delete(temp_root);
    cJSON_Delete(root);
}

// 解析网关下发指令
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf)
{
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

    uint64_t now_ts = Time_To_Unix();
    int64_t time_diff = llabs((int64_t)now_ts - (int64_t)curr_ts);
    if(time_diff > TIME_VALID_SEC)  goto exit;

    if(curr_seq <= g_last_cmd_seq)     goto exit;
    g_last_cmd_seq = curr_seq;

    memset(tcp_sign_buf_2048, 0, sizeof(tcp_sign_buf_2048));
    uint8_t calc_hash[32] = {0};
    char calc_sign[9] = {0};

    char ts_cmd_str[21] = {0};
    uint64_to_str(curr_ts, ts_cmd_str);

    if(strcmp(type->valuestring, "task_order") == 0)
    {
        cJSON *task_info = cJSON_GetObjectItem(root, "task_info");
        if(!task_info ||  task_info->type != cJSON_Array) goto exit;
        cJSON *task = cJSON_GetArrayItem(task_info, 0);
        if(!task) goto exit;
        cJSON *inj_id   = cJSON_GetObjectItem(task, "inj_id");
        cJSON *inj_v    = cJSON_GetObjectItem(task, "inj_v");
        cJSON *task_id  = cJSON_GetObjectItem(task, "task_id");
        cJSON *task_type= cJSON_GetObjectItem(task, "task_type");
        if(!inj_id || !inj_v || !task_id || !task_type) goto exit;

        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"inj_id\":%d,\"inj_v\":%d,\"seq\":%lu,\"task_id\":\"%s\",\"task_type\":%d,\"ts\":%s,\"type\":\"%s\"}",
            DEVICE_ID,
            inj_id->valueint,
            inj_v->valueint,
            (unsigned long)curr_seq,
            task_id->valuestring,
            task_type->valueint,
            ts_cmd_str,
            "task_order"
        );
        LOG("TCP Sorted task_order JSON:\r\n%s\r\n", tcp_sign_buf_2048);
    }
    else if(strcmp(type->valuestring, "state_request") == 0)
    {
        snprintf(tcp_sign_buf_2048, sizeof(tcp_sign_buf_2048),
            "{\"device_id\":\"%s\",\"seq\":%lu,\"ts\":%s,\"type\":\"%s\"}",
            DEVICE_ID,
            (unsigned long)curr_seq,
            ts_cmd_str,
            "state_request"
        );
        LOG("TCP Sorted state_request JSON:\r\n%s\r\n", tcp_sign_buf_2048);
    }

    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)tcp_sign_buf_2048, strlen(tcp_sign_buf_2048));
    hmac_sha256_final(&hmac, calc_hash);
    snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X", 
            calc_hash[0], calc_hash[1], calc_hash[2], calc_hash[3]);

    if(strcmp(sign->valuestring, calc_sign) != 0) goto exit;

    if(strcmp(type->valuestring, "task_order") == 0)
    {

    }
    if(strcmp(type->valuestring, "state_request") == 0)
    {
        tcp_send_heartbeat();
    }

exit:
    cJSON_Delete(root);
}

// TCP 发送字符串
static void tcp_send_str(struct tcp_pcb *tpcb, const char *str)
{
    if(tpcb == NULL || str == NULL) return;
    tcp_write(tpcb, str, strlen(str), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

// TCP 连接成功后立即发送 device_online 上线报文
static void tcp_send_device_online(void)
{
    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Time_To_Unix();
    char mac_str[32] = {0};
    uint8_t *mac = gnetif.hwaddr;

    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    memset(sign_buf_online, 0, sizeof(sign_buf_online));
    char ts_online_str[21] = {0};
    uint64_to_str(ts, ts_online_str);

    snprintf(sign_buf_online, sizeof(sign_buf_online),
            "{\"cur_ip\":\"%s\",\"firmware_ver\":\"%s\",\"is_reboot\":true,\"mac\":\"%s\",\"model\":\"%s\",\"reason\":0,\"seq\":%lu,\"sn\":\"%s\",\"ts\":%s,\"type\":\"%s\"}",
            ip4addr_ntoa(netif_ip4_addr(&gnetif)),
            DEVICE_FW_VER,
            mac_str,
            DEVICE_MODEL,
            (unsigned long)g_seq,
            DEVICE_SN,
            ts_online_str,
            "device_online"); 

    uint8_t hash[32] = {0};
    hmac_sha256_init(&hmac, (const uint8_t *)HMAC_KEY, strlen(HMAC_KEY));
    hmac_sha256_update(&hmac, (const uint8_t *)sign_buf_online, strlen(sign_buf_online));
    hmac_sha256_final(&hmac, hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    cJSON_AddStringToObject(root, "type", "device_online");
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "sn", DEVICE_SN);
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(root, "firmware_ver", DEVICE_FW_VER);
    cJSON_AddStringToObject(root, "cur_ip", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
    cJSON_AddBoolToObject(root, "is_reboot", cJSON_True);
    cJSON_AddNumberToObject(root, "reason", 0);
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "seq", g_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);

    char *str = cJSON_PrintUnformatted(root);
    if(str)
    {
        tcp_send_str(tcp_pcb, str);
        LOG("device_online : %s", str);
        free(str);
        g_seq++;
    }
    cJSON_Delete(root);
}

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <stdio.h>

void print_local_ip_2(void)
{
    ip_addr_t *ip;
    uint8_t last;
    while(1)
    {
        ip = netif_ip4_addr(netif_default);
        if(ip4_addr_isany(ip))
        {
            osDelay(200);
            continue;
        }
        last = ip4_addr_get_u32(ip) & 0xFF;
        if(last != 0 && last != 255)
        {
            printf("AutoIP OK: %s\r\n", ipaddr_ntoa(ip));
            break;
        }
        osDelay(200);
    }
}

void udp_echo_task(void *arg)
{
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char recv_buf[1024];
    int recv_len;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0)
    {
        printf("UDP socket create failed\r\n");
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5000);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("UDP bind failed\r\n");
        closesocket(sockfd);
        vTaskDelete(NULL);
        return;
    }

    printf("UDP server listening on port 5000...\r\n");
    print_local_ip_2();

    while(1)
    {
        recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf) - 1, 0,
                            (struct sockaddr *)&client_addr, &addr_len);

        if(recv_len > 0)
        {
            recv_buf[recv_len] = '\0';
            printf("Recv from %s:%d, len=%d: %s\r\n",
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port),
                   recv_len, recv_buf);

            sendto(sockfd, recv_buf, recv_len, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
        else
        {
            osDelay(10);
        }
    }
}
