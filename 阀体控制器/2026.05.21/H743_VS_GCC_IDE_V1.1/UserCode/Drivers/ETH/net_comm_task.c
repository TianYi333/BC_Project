#include "net_comm_task.h"


static char tcp_rx_buf[RX_BUF_SIZE];
static char tcp_cfg_rx_buf[RX_BUF_SIZE];
static struct tcp_pcb *tcp_pcb = NULL;
static struct tcp_pcb *tcp_cfg_pcb = NULL;
static uint32_t g_seq = 1;  // 全局自增序列号，上电从 1 开始
static uint32_t g_last_gw_seq = 0;            // 上位机 -> 设备：发现报文 seq
static uint32_t g_last_cfg_seq = 0;           // 上位机 -> 设备：配网报文 seq
static uint32_t g_last_cmd_seq = 0;           // 上位机 -> 设备：指令报文 seq 

static uint8_t  g_network_configured = 0;         // 配网成功标志：0=未配网 1=已配网
static uint32_t g_last_heartbeat_tick = 0;        // 心跳计时

// 全局网络参数（由上位机 TCP9530 配网下发）
static ip4_addr_t  g_server_ip;                // 上位机业务服务器IP（动态保存）
static NetConfig_t g_net_cfg;
// 网络独立数据库句柄
struct fdb_kvdb net_kvdb;


static void udp_recv_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf);
static void tcp_send_str(struct tcp_pcb *tpcb, const char *str);
static void tcp_send_heartbeat(void);


//===== TCP配网服务端回调 =====
static err_t tcp_cfg_accept_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t tcp_cfg_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_cfg_parse_config(struct tcp_pcb *tpcb, char *buf);
static void tcp_send_device_online(void);

// 网络数据库初始化（独立、安全、带线程锁）
int8_t init_net_db(void)
{
    fdb_err_t result;
    // 网络没有默认KV表，所以直接给0
    struct fdb_default_kv default_kv = {0};

    // 线程锁必须加（因为是独立数据库）
    fdb_kvdb_control(&net_kvdb, FDB_KVDB_CTRL_SET_LOCK, (void *)lock);
    fdb_kvdb_control(&net_kvdb, FDB_KVDB_CTRL_SET_UNLOCK, (void *)unlock);

    // 初始化：netdb = 网络数据库，ef_kvdb1 = 分区
    result = fdb_kvdb_init(&net_kvdb, "netdb", "ef_kvdb1", &default_kv, NULL);

    if (result != FDB_NO_ERR) {
        return -1;
    }

    return 0;
}

//=====================================================================
// 读取配网参数 from FlashDB
//=====================================================================
void net_config_init(void)
{
    // 正确：fdb_blob_t 是指针，动态分配/直接定义结构体，再取地址
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;

    memset(&g_net_cfg, 0, sizeof(NetConfig_t));

    blob->buf = (uint8_t *)&g_net_cfg;
    blob->size = sizeof(NetConfig_t);

    size_t len = fdb_kv_get_blob(&net_kvdb, "net_cfg", blob);

    if (len == sizeof(NetConfig_t) && g_net_cfg.configured == 1)//configured == 1 表示 已经配过网
    {
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip, g_net_cfg.ip[0], g_net_cfg.ip[1], g_net_cfg.ip[2], g_net_cfg.ip[3]);
        IP4_ADDR(&mask, g_net_cfg.netmask[0], g_net_cfg.netmask[1], g_net_cfg.netmask[2], g_net_cfg.netmask[3]);
        IP4_ADDR(&gw, g_net_cfg.gateway[0], g_net_cfg.gateway[1], g_net_cfg.gateway[2], g_net_cfg.gateway[3]);

        netif_set_ipaddr(&gnetif, &ip);
        netif_set_netmask(&gnetif, &mask);
        netif_set_gw(&gnetif, &gw);

        g_server_ip = gw;
        g_network_configured = 1;
        LOG("从Flash加载配网成功,启动TCP");
    }
    else
    {
        g_network_configured = 0;
        LOG("未配网,启动UDP广播");
    }
}

//=====================================================================
// 保存配网参数 to FlashDB
//=====================================================================
static void net_config_save(void)
{
    // 标记已配网
    g_net_cfg.configured = 1;

    // 构造 blob（完全适配你的库）
    struct fdb_blob blob_obj;
    fdb_blob_t blob = &blob_obj;
    blob->buf = (uint8_t *)&g_net_cfg;
    blob->size = sizeof(NetConfig_t);

    // 3 个参数，正确调用
    fdb_kv_set_blob(&net_kvdb, "net_cfg", blob);

    LOG("配网参数已保存至Flash");
}

//=====================================================================
/**
 * @brief  UDP设备发现任务，端口9527（始终运行）
 * @note   监听上位机UDP广播发现报文，回复设备信息
 *         仅开启广播接收，AutoIP模式无法使用组播
 */
//=====================================================================
void udp_discover_task(void *arg)
{
  // 1. 创建UDP
  struct udp_pcb *upcb = udp_new();

  // 2. 如果创建成功
  if (upcb != NULL)
  {
    // 绑定端口 9527，IP_ADDR_ANY表示接收所有接口的数据
    udp_bind(upcb, IP_ADDR_ANY, UDP_LISTEN_PORT);

    // 开启广播
    upcb->so_options |= SOF_BROADCAST;

   //因无固定IP所以无法使用组播

    // 注册接收回调！！！
    udp_recv(upcb, udp_recv_callback, NULL);
  }

  // 3. 死循环，但不占CPU
  while (1)
  {
    osDelay(1000);
  }
}

//=====================================================================
/**
 * @brief  UDP接收回调函数
 * @param  addr: 上位机IP地址
 * @param  port: 上位机端口
 * @note   1. 校验type/domain/ts/seq/sign安全字段
 *         2. 时间戳±60s有效、seq递增防重放、HMAC签名校验
 *         3. 校验通过后，单播回复discover_response设备信息给上位机
 */
//=====================================================================
static void udp_recv_callback(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    // 无数据直接返回
    if (p == NULL) return;

    // 1. 把收到的数据拷贝到缓冲区
    char buf[512] = {0};
    int recv_len = p->len < 511 ? p->len : 511;
    memcpy(buf, p->payload, recv_len);

    // 2. 解析JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        pbuf_free(p);
        return;
    }

    // 3. 读取所有关键字段
    cJSON *type            = cJSON_GetObjectItem(root, "type");
    cJSON *domain          = cJSON_GetObjectItem(root, "domain");
    cJSON *gateway_mac     = cJSON_GetObjectItem(root, "gateway_mac");
    cJSON *ts              = cJSON_GetObjectItem(root, "ts");
    cJSON *seq             = cJSON_GetObjectItem(root, "seq");
    cJSON *sign            = cJSON_GetObjectItem(root, "sign");
    

    // 基础合法性校验
    if (!type || type->type != cJSON_String)                goto err_exit;
    if (!domain || domain->type != cJSON_String)            goto err_exit;
    if (!gateway_mac || gateway_mac->type != cJSON_String)  goto err_exit;
    if (!ts || ts->type != cJSON_Number)                    goto err_exit;
    if (!seq || seq->type != cJSON_Number)                  goto err_exit;
    if (!sign || sign->type != cJSON_String)                goto err_exit;
    
    if (strcmp(type->valuestring, "discover_request") != 0) goto err_exit;
    if (strcmp(domain->valuestring, "FACTORY_OIL") != 0)    goto err_exit;

    // 时间戳校验：与当前时间差 <60秒
    uint64_t now_ts = Time_To_Unix();
    int64_t  time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
    if (time_diff > TIME_VALID_SEC)  goto err_exit;

    // 上位机 seq 必须比上一次大（防重放）
    uint32_t curr_gw_seq = (uint32_t)seq->valuedouble;
    if (curr_gw_seq <= g_last_gw_seq ) goto err_exit;
    g_last_gw_seq = curr_gw_seq;

    // 上位机签名校验
    char gw_sign_buf[512] = {0};
    //按照字典序排序
    snprintf(gw_sign_buf,sizeof(gw_sign_buf)-1,
        "domain=%s&gateway_mac=%s&seq=%lu&ts=%llu&type=%s",
        domain->valuestring,
        gateway_mac->valuestring,
        (unsigned long)curr_gw_seq,
        (unsigned long long)ts->valuedouble,
        "discover_request"
    );

    uint8_t gw_calc_hash[32] = {0};
    HMAC_SHA256_Soft((uint8_t *)HMAC_KEY, strlen(HMAC_KEY),
                     (uint8_t *)gw_sign_buf, strlen(gw_sign_buf),
                     gw_calc_hash);

    char gw_calc_sign[9] = {0};
    snprintf(gw_calc_sign, sizeof(gw_calc_sign), "%02X%02X%02X%02X",
            gw_calc_hash[0], gw_calc_hash[1],
            gw_calc_hash[2], gw_calc_hash[3]);

    // 签名不匹配 → 非法报文
    if (strcmp(sign->valuestring, gw_calc_sign) != 0) {
        goto err_exit;
    }


    // 所有校验通过 → 组装回复（独立代码块）
    {
        // 打印收到的网关MAC
        // printf("Gateway MAC: %s\r\n", gateway_mac->valuestring);
        cJSON *resp = cJSON_CreateObject();

        // 类型
        cJSON_AddStringToObject(resp, "type", "discover_response");

        // 本机MAC地址
        uint8_t *mac_addr = gnetif.hwaddr;
        char mac_str[32];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac_addr[0], mac_addr[1], mac_addr[2],
                mac_addr[3], mac_addr[4], mac_addr[5]);
        cJSON_AddStringToObject(resp, "mac", mac_str);

        // 设备序列号
        cJSON_AddStringToObject(resp, "sn", "OIL-2026-0001");

        // 设备型号
        cJSON_AddStringToObject(resp, "model", "LUB-CTRL-V1.0");

        // 固件版本
        cJSON_AddStringToObject(resp, "firmware_ver", "1.0.3");

        // 当前设备IP（自动获取，不用手动填）
        cJSON_AddStringToObject(resp, "cur_ip", ip4addr_ntoa(netif_ip4_addr(&gnetif)));

        // 时间戳
        uint64_t real_ts = Time_To_Unix();  // 调用函数获取RTC实时时间戳
        cJSON_AddNumberToObject(resp, "ts", real_ts);

        // 序列号（全局变量++）
        cJSON_AddNumberToObject(resp, "seq", g_seq);

        // 设备端签名
        char sign_buf[512] = {0};
        //按照字典序排序
        snprintf(sign_buf, sizeof(sign_buf),
            "cur_ip=%s&firmware_ver=%s&mac=%s&model=%s&sn=%s&seq=%u&ts=%llu&type=%s",
            ip4addr_ntoa(netif_ip4_addr(&gnetif)),
            DEVICE_FW_VER,
            mac_str,
            DEVICE_MODEL,
            DEVICE_SN,
            (unsigned int)g_seq,
            (unsigned long long)real_ts,
            "discover_response"
        );

        uint8_t sha256_result[32] = {0};
        HMAC_SHA256_Soft((uint8_t *)HMAC_KEY, strlen(HMAC_KEY),
                         (uint8_t *)sign_buf, strlen(sign_buf),
                         sha256_result);

        char sign_str[9] = {0};//8 个字符+1 个结束符\0
        snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
                sha256_result[0],
                sha256_result[1],
                sha256_result[2],
                sha256_result[3]);

        cJSON_AddStringToObject(resp, "sign", sign_str);

        // 6. 转为JSON字符串（自动带 {}，标准格式）
        char *json_reply = cJSON_PrintUnformatted(resp);
        if (json_reply != NULL)
        {
            // 发送回复给上位机
            struct pbuf *p_tx = pbuf_alloc(PBUF_TRANSPORT, strlen(json_reply), PBUF_POOL);
            if (p_tx != NULL)
            {
                memcpy(p_tx->payload, json_reply, strlen(json_reply));
                // udp_sendto(upcb, p_tx, addr, port);//单播

                //广播回复给局域网内所有设备，满足协议要求
                // 配置广播地址 255.255.255.255
                ip_addr_t broadcast_addr;
                IP4_ADDR(&broadcast_addr, 255, 255, 255, 255);
                // 广播发送，端口固定9527
                udp_sendto(upcb, p_tx, &broadcast_addr, UDP_LISTEN_PORT);
                pbuf_free(p_tx);
            }
            free(json_reply);
        }

        cJSON_Delete(resp);
        g_seq++;
    }

err_exit:
    cJSON_Delete(root);
    pbuf_free(p);
}

//=====================================================================
/**
 * @brief  TCP配网服务端任务，端口9530
 * @note   1. 设备作为TCP服务端，监听9530端口，等待上位机主动连接
 *         2. 接收上位机下发的静态IP、子网掩码、网关，自动配置网卡
 *         3. 完成从AutoIP动态IP → 静态固定IP的切换
 */
//=====================================================================
void tcp_config_server_task(void *arg)
{
    while (1)
    {
        // 网口未上电/未插网线，延时重试
        if ((gnetif.flags & NETIF_FLAG_UP) == 0 || (gnetif.flags & NETIF_FLAG_LINK_UP) == 0)
        {
            osDelay(500);
            continue;
        }

        // 未创建TCP服务端，则创建并监听
        if (tcp_cfg_pcb == NULL)
        {
            tcp_cfg_pcb = tcp_new();
            if (tcp_cfg_pcb != NULL)
            {
                tcp_bind(tcp_cfg_pcb, IP_ADDR_ANY, TCP_CONFIG_PORT); // 绑定9530
                tcp_cfg_pcb = tcp_listen(tcp_cfg_pcb); // 进入监听模式
                tcp_accept(tcp_cfg_pcb, tcp_cfg_accept_cb);// 注册连接接受回调
            }
        }
        osDelay(1000);
    }
}

//TCP配网服务端：接受上位机连接回调
static err_t tcp_cfg_accept_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if (err != ERR_OK) return err;
    tcp_recv(tpcb, tcp_cfg_recv_cb);// 注册接收回调
    return ERR_OK;
}

// TCP配网服务端：接收上位机下发的配网参数回调
static err_t tcp_cfg_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (err != ERR_OK || p == NULL)
    {
        tcp_close(tpcb);
        return ERR_CLSD;
    }

    // 拷贝数据到缓冲区
    memset(tcp_cfg_rx_buf, 0, RX_BUF_SIZE);
    uint16_t recv_len = p->len < (RX_BUF_SIZE - 1) ? p->len : (RX_BUF_SIZE - 1);
    memcpy(tcp_cfg_rx_buf, p->payload, recv_len);

    // 解析配网报文
    tcp_cfg_parse_config(tpcb, tcp_cfg_rx_buf);

    tcp_recved(tpcb, p->len);
    pbuf_free(p);
    return ERR_OK;
}

//=====================================================================
/**
 * @brief  解析上位机配网报文（无回复）
 * @格式
{
  "type": "config_set",
  "target_mac": "11:22:33:44:55:66",
  "target_sn": "OIL-2026-0001",
  "ip": "192.168.10.101",
  "netmask": "255.255.255.0",
  "gateway": "192.168.10.1",
  "ts": 1746601205,
  "seq": 1002,
  "sign": "11B8E7D2"
}
 */
//=====================================================================
static void tcp_cfg_parse_config(struct tcp_pcb *tpcb, char *buf)
{
    // 1. 解析JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        cJSON_Delete(root);
        return;
    }

    // 2. 读取所有字段
    cJSON *type        = cJSON_GetObjectItem(root, "type");
    cJSON *target_mac  = cJSON_GetObjectItem(root, "target_mac");
    cJSON *target_sn   = cJSON_GetObjectItem(root, "target_sn");
    cJSON *ip          = cJSON_GetObjectItem(root, "ip");
    cJSON *netmask     = cJSON_GetObjectItem(root, "netmask");
    cJSON *gateway     = cJSON_GetObjectItem(root, "gateway");
    cJSON *ts          = cJSON_GetObjectItem(root, "ts");
    cJSON *seq         = cJSON_GetObjectItem(root, "seq");
    cJSON *sign        = cJSON_GetObjectItem(root, "sign");

    // 3. 字段非空检查
    if (!type || !target_mac || !target_sn || !ip || !netmask || !gateway ||
        !ts || !seq || !sign)
    {
        cJSON_Delete(root);
        return;
    }
 
    // 4. 检查类型必须是 config_set
    if (strcmp(type->valuestring, "config_set") != 0)
    {
        cJSON_Delete(root);
        return;
    }

    // 5. 获取本机MAC字符串
    char local_mac[32] = {0};
    snprintf(local_mac, sizeof(local_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
            gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
            gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);

    // 6. 检查MAC是否是发给本机
    if (strcmp(target_mac->valuestring, local_mac) != 0)
    {
        cJSON_Delete(root);
        return;
    }

    // 7. 检查SN是否匹配
    if (strcmp(target_sn->valuestring, DEVICE_SN) != 0)
    {
        cJSON_Delete(root);
        return;
    }

    // 8. 时间戳校验 ±60s
    uint64_t now_ts = Time_To_Unix();
    int64_t time_diff = llabs((int64_t)now_ts - (int64_t)ts->valuedouble);
    if (time_diff > TIME_VALID_SEC)
    {
        cJSON_Delete(root);
        return;
    }

    // 9. 序列号防重放（必须递增）
    uint32_t curr_seq = (uint32_t)seq->valuedouble;
    if (curr_seq <= g_last_cfg_seq)
    {
        cJSON_Delete(root);
        return;
    }
    g_last_cfg_seq = curr_seq;

    // 10. 签名校验：字典序拼接
    char sign_buf[512] = {0};
    snprintf(sign_buf, sizeof(sign_buf),
            "gateway=%s&ip=%s&netmask=%s&seq=%lu&target_mac=%s&target_sn=%s&ts=%llu&type=%s",
            gateway->valuestring,
            ip->valuestring,
            netmask->valuestring,
            (unsigned long)curr_seq,
            target_mac->valuestring,
            target_sn->valuestring,
            (unsigned long long)ts->valuedouble,
            "config_set");

    // 计算 HMAC-SHA256
    uint8_t calc_hash[32] = {0};
    HMAC_SHA256_Soft((uint8_t *)HMAC_KEY, strlen(HMAC_KEY),
                     (uint8_t *)sign_buf, strlen(sign_buf),
                     calc_hash);

    // 转成 8 位 HEX 字符串
    char calc_sign[9] = {0};
    snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X",
            calc_hash[0], calc_hash[1], calc_hash[2], calc_hash[3]);

    // 比较签名
    if (strcmp(sign->valuestring, calc_sign) != 0)
    {
        cJSON_Delete(root);
        return;
    }

    // 所有校验成功 → 设置静态IP
    ip4_addr_t ip_addr, mask_addr, gw_addr;
    ipaddr_aton(ip->valuestring, &ip_addr);
    ipaddr_aton(netmask->valuestring, &mask_addr);
    ipaddr_aton(gateway->valuestring, &gw_addr);

    netif_set_ipaddr(&gnetif, &ip_addr);
    netif_set_netmask(&gnetif, &mask_addr);
    netif_set_gw(&gnetif, &gw_addr);

    g_server_ip = gw_addr;  // 直接把网关IP当作服务器IP

    g_network_configured = 1;                            // 标记配网成功

    // 保存到 Flash（适配你的 FlashDB 库）
    memcpy(g_net_cfg.ip, &ip_addr.addr, 4);
    memcpy(g_net_cfg.netmask, &mask_addr.addr, 4);
    memcpy(g_net_cfg.gateway, &gw_addr.addr, 4);

    net_config_save();

    LOG("配网成功并保存Flash");

    cJSON_Delete(root);
}

//=====================================================================
// TCP 客户端任务（配网成功才连接，连接后自动发心跳）
//=====================================================================
void tcp_client_task(void *arg)
{
    while (1)
    {
        // 网口未就绪
        if ((gnetif.flags & NETIF_FLAG_UP) == 0 || (gnetif.flags & NETIF_FLAG_LINK_UP) == 0)
        {
            osDelay(500);
            continue;
        }

        // 未配网成功 → 不连接
        if (g_network_configured == 0)
        {
            osDelay(500);
            continue;
        }

        // 未连接则创建连接
        if (tcp_pcb == NULL)
        {
            tcp_pcb = tcp_new();
            if (tcp_pcb != NULL)
            {
                tcp_connect(tcp_pcb, &g_server_ip, TCP_SERVER_PORT, tcp_connected_cb);
            }
        }

        // 已连接 → 每30秒发一次心跳
        if (tcp_pcb != NULL && g_network_configured == 1)
        {
            uint32_t now = osKernelGetTickCount();
            if (now - g_last_heartbeat_tick >= HEARTBEAT_PERIOD_MS)
            {
                tcp_send_heartbeat(); // 按协议发心跳
                g_last_heartbeat_tick = now;
            }
        }

        osDelay(500);
    }
}

//=====================================================================
// TCP 连接成功回调
//=====================================================================
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    if (err != ERR_OK)
    {
        tcp_pcb = NULL;
        return err;
    }
    tcp_recv(tpcb, tcp_recv_cb);

    // 连接成功立刻发上线报文
    tcp_send_device_online();
    return ERR_OK;
}

//=====================================================================
// TCP 接收回调
//=====================================================================
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if (err != ERR_OK || p == NULL)
    {
        tcp_close(tpcb);
        tcp_pcb = NULL;
        return ERR_CLSD;
    }

    memset(tcp_rx_buf, 0, RX_BUF_SIZE);
    uint16_t recv_len = p->len < (RX_BUF_SIZE - 1) ? p->len : (RX_BUF_SIZE - 1);
    memcpy(tcp_rx_buf, p->payload, recv_len);

    // 解析网关指令
    tcp_parse_cmd(tpcb, tcp_rx_buf);

    tcp_recved(tpcb, p->len);
    pbuf_free(p);
    return ERR_OK;
}

//=====================================================================
// 按协议发送心跳报文
//=====================================================================
static void tcp_send_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();// 最外层  { }  大对象
    cJSON *cap = cJSON_CreateArray();// capability 是数组  [ ]

    uint64_t ts = Time_To_Unix();  // 时间戳（签名和报文用同一个）
    char sign_buf[2048] = {0};     // 签名字符串缓冲区

    // 8个注油器标准格式
    for (int i = 1; i <= 8; i++)
    {
        // 每个注油器对应一个JSON对象 { ... }
        cJSON *item = cJSON_CreateObject();// 每个注油器都是 { }
        // 注油器ID：1~8
        cJSON_AddNumberToObject(item, "inj_id", i);
        // 注油器最大流量：固定6
        cJSON_AddNumberToObject(item, "inj_jr", 6);
        // 注油器状态：0=正常
        cJSON_AddNumberToObject(item, "inj_st", 0);
        // 注油器目标流量：800
        cJSON_AddNumberToObject(item, "inj_tr", 800);
        // 注油器总运行时间：1200
        cJSON_AddNumberToObject(item, "inj_ts", 1200);
        // 注油器阀门状态：10=默认
        cJSON_AddNumberToObject(item, "inj_vs", 10);
        // 把这个注油器对象加入 capability 数组 [ ]
        cJSON_AddItemToArray(cap, item);
    }

    //按字典序拼接所有字段
    snprintf(sign_buf, sizeof(sign_buf),
        // 公共字段（字典序）
        "device_id=%s"

        // 8个注油器（按索引展开，字典序）
        "&inj_id1=1&inj_jr1=6&inj_st1=0&inj_tr1=800&inj_ts1=1200&inj_vs1=10"
        "&inj_id2=2&inj_jr2=6&inj_st2=0&inj_tr2=800&inj_ts2=1200&inj_vs2=10"
        "&inj_id3=3&inj_jr3=6&inj_st3=0&inj_tr3=800&inj_ts3=1200&inj_vs3=10"
        "&inj_id4=4&inj_jr4=6&inj_st4=0&inj_tr4=800&inj_ts4=1200&inj_vs4=10"
        "&inj_id5=5&inj_jr5=6&inj_st5=0&inj_tr5=800&inj_ts5=1200&inj_vs5=10"
        "&inj_id6=6&inj_jr6=6&inj_st6=0&inj_tr6=800&inj_ts6=1200&inj_vs6=10"
        "&inj_id7=7&inj_jr7=6&inj_st7=0&inj_tr7=800&inj_ts7=1200&inj_vs7=10"
        "&inj_id8=8&inj_jr8=6&inj_st8=0&inj_tr8=800&inj_ts8=1200&inj_vs8=10"

        // 剩余公共字段
        "&run_status=0"
        "&seq=%lu"
        "&ts=%llu"
        "&type=heartbeat",
        DEVICE_ID,
        (unsigned long)g_seq,
        (unsigned long long)ts
    );

    // 将 capability 数组添加到最外层根对象 { }
    cJSON_AddItemToObject(root, "capability", cap);
    cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
    cJSON_AddNumberToObject(root, "run_status", 0);
    cJSON_AddNumberToObject(root, "seq", g_seq);

    uint8_t hash[32];
    HMAC_SHA256_Soft((uint8_t *)HMAC_KEY, strlen(HMAC_KEY), (uint8_t *)sign_buf, strlen(sign_buf), hash);
    
    char sign_str[9];
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X", hash[0], hash[1], hash[2], hash[3]);

    cJSON_AddStringToObject(root, "sign", sign_str);
    cJSON_AddNumberToObject(root, "ts", Time_To_Unix());
    cJSON_AddStringToObject(root, "type", "heartbeat");

    // 发送
    char *str = cJSON_PrintUnformatted(root);
    if (str)
    {
        tcp_send_str(tcp_pcb, str);
        free(str);
        g_seq++;
    }
    cJSON_Delete(root);
}

//=====================================================================
// 解析网关下发指令
//=====================================================================
static void tcp_parse_cmd(struct tcp_pcb *tpcb, char *buf)
{
    // 解析JSON
    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    // 读取必选校验字段
    cJSON *type    = cJSON_GetObjectItem(root, "type");
    cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    cJSON *seq     = cJSON_GetObjectItem(root, "seq");
    cJSON *ts      = cJSON_GetObjectItem(root, "ts");
    cJSON *sign    = cJSON_GetObjectItem(root, "sign");

    // 基础非空校验（必须都存在）
    if (!type || type->type != cJSON_String)            goto exit;
    if (!device_id || device_id->type != cJSON_String)  goto exit;
    if (!seq || seq->type != cJSON_Number)              goto exit;
    if (!ts || ts->type != cJSON_Number)                goto exit;
    if (!sign || sign->type != cJSON_String)            goto exit;

    // 设备ID校验（必须是 OIL_DEV_101）
    if (strcmp(device_id->valuestring, DEVICE_ID) != 0) goto exit;

    uint32_t curr_seq = (uint32_t)seq->valuedouble;
    uint64_t curr_ts  = (uint64_t)ts->valuedouble;

    // 时间戳校验 ±60s
    uint64_t now_ts = Time_To_Unix();
    int64_t time_diff = llabs((int64_t)now_ts - (int64_t)curr_ts);
    if (time_diff > TIME_VALID_SEC)  goto exit;

    // 序列号防重放（必须递增）
    if (curr_seq <= g_last_cmd_seq)     goto exit;
    g_last_cmd_seq = curr_seq;

    char sign_buf[1024] = {0};
    uint8_t calc_hash[32] = {0};
    char calc_sign[9] = {0};
    // 网关任务下发
    if (strcmp(type->valuestring, "task_order") == 0)
    {
        // 读取任务数组
        cJSON *task_info = cJSON_GetObjectItem(root, "task_info");
        //判断task_info对象 是不是 数组 []
        if (!task_info ||  task_info->type != cJSON_Array) goto exit;

        // 取数组第1个任务（协议里是单个任务）
        cJSON *task = cJSON_GetArrayItem(task_info, 0);
        if (!task) goto exit;
        // 读取任务字段
        cJSON *inj_id   = cJSON_GetObjectItem(task, "inj_id");
        cJSON *inj_v    = cJSON_GetObjectItem(task, "inj_v");
        cJSON *task_id  = cJSON_GetObjectItem(task, "task_id");
        cJSON *task_type= cJSON_GetObjectItem(task, "task_type");

        // 非空校验
        if (!inj_id || !inj_v || !task_id || !task_type) goto exit;

        snprintf(sign_buf, sizeof(sign_buf),
            "device_id=%s"
            "&inj_id=%d&inj_v=%d"
            "&seq=%lu&task_id=%s&task_type=%d"
            "&ts=%llu&type=task_order",
            DEVICE_ID,
            inj_id->valueint,
            inj_v->valueint,
            (unsigned long)curr_seq,
            task_id->valuestring,
            task_type->valueint,
            (unsigned long long)curr_ts
        );

    }
    else if(strcmp(type->valuestring, "state_request") == 0)
    {
        snprintf(sign_buf, sizeof(sign_buf),
            "device_id=%s&seq=%lu&ts=%llu&type=state_request",
            DEVICE_ID,
            (unsigned long)curr_seq,
            (unsigned long long)curr_ts
        );
    }

    // 验签
    HMAC_SHA256_Soft((uint8_t *)HMAC_KEY, strlen(HMAC_KEY), (uint8_t *)sign_buf, strlen(sign_buf), calc_hash);
    snprintf(calc_sign, sizeof(calc_sign), "%02X%02X%02X%02X", calc_hash[0], calc_hash[1], calc_hash[2], calc_hash[3]);

    if(strcmp(sign->valuestring, calc_sign) != 0) goto exit;

    if(strcmp(type->valuestring, "task_order") == 0)
    {
        // 执行任务
    }
    if(strcmp(type->valuestring, "state_request") == 0)
    {
        tcp_send_heartbeat();
    }

exit:
    cJSON_Delete(root);
}

//=====================================================================
// TCP 发送字符串
//=====================================================================
static void tcp_send_str(struct tcp_pcb *tpcb, const char *str)
{
    if (tpcb == NULL || str == NULL) return;
    // 把数据写入TCP发送缓冲区
    tcp_write(tpcb, str, strlen(str), TCP_WRITE_FLAG_COPY);
    // 立即触发发送（不等待缓冲区满）
    tcp_output(tpcb);
}

//=====================================================================
// TCP 连接成功后立即发送 device_online 上线报文
//=====================================================================
static void tcp_send_device_online(void)
{
    cJSON *root = cJSON_CreateObject();
    uint64_t ts = Time_To_Unix();
    char sign_buf[2048] = {0};
    char mac_str[32] = {0};
    uint8_t *mac = gnetif.hwaddr;

    // 组装MAC地址字符串
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // ==============================
    // 【字典序】拼接签名字符串
    // ==============================
    snprintf(sign_buf, sizeof(sign_buf),
             "cur_ip=%s&firmware_ver=%s&is_reboot=true&mac=%s&model=%s&reason=0&seq=%lu&sn=%s&ts=%llu&type=%s",
             ip4addr_ntoa(netif_ip4_addr(&gnetif)),  // cur_ip
             DEVICE_FW_VER,                           // firmware_ver
             mac_str,                                 // mac
             DEVICE_MODEL,                            // model
             (unsigned long)g_seq,                    // seq
             DEVICE_SN,                               // sn
             (unsigned long long)ts,                  // ts
             DEVICE_ID);                              // 冗余兼容，不影响

    // 计算 HMAC-SHA256 签名
    uint8_t hash[32] = {0};
    HMAC_SHA256_Soft((uint8_t *)HMAC_KEY, strlen(HMAC_KEY),
                     (uint8_t *)sign_buf, strlen(sign_buf), hash);

    char sign_str[9] = {0};
    snprintf(sign_str, sizeof(sign_str), "%02X%02X%02X%02X",
             hash[0], hash[1], hash[2], hash[3]);

    // ==============================
    // 组装JSON（完全匹配你给的格式）
    // ==============================
    cJSON_AddStringToObject(root, "type", "device_online");
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "sn", DEVICE_SN);
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(root, "firmware_ver", DEVICE_FW_VER);
    cJSON_AddStringToObject(root, "cur_ip", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
    cJSON_AddBoolToObject(root, "is_reboot", cJSON_True);  // true
    cJSON_AddNumberToObject(root, "reason", 0);            // 0
    cJSON_AddNumberToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "seq", g_seq);
    cJSON_AddStringToObject(root, "sign", sign_str);

    // 发送报文
    char *str = cJSON_PrintUnformatted(root);
    if (str)
    {
        tcp_send_str(tcp_pcb, str);
        LOG("device_online 已发送: %s", str);
        free(str);
        g_seq++;
    }

    cJSON_Delete(root);
}



