/**
  ******************************************************************************
  * @file    sensor_hub.c
  * @brief   8 路 RS485 终端传感器采集层实现
  ******************************************************************************
  */
#include "sensor_hub.h"
#include "net_comm_task.h"   /* tcp_send_sensor_data() / sensorhub_get_mode() */
#include "debug_uart.h"      /* 临时调试口（SH_DEBUG_UART_ID） */
#include <string.h>
#include <stdio.h>
#include "semphr.h"          /* FC06 写应答同步信号量 */

/* ============================ 端口映射表 ============================
 * sensor_id 1..8（即 CH1..CH8）与 UART、RS485 方向引脚（DE/RE）的真实对应关系。
 * 各路是否用 DMA 由具体 UART 决定：UART7/8 无 DMA（中断接收），其余 6 路有 DMA。
 * 映射以板级硬件接线为准（见下方逐行标注），改接线只需同步改本表。
 */
typedef struct {
    UART_HandleTypeDef *huart;
    GPIO_TypeDef       *de_port;
    uint16_t            de_pin;
    uint8_t             use_dma;      /* 1=DMA接收，0=中断接收 */
    uint8_t             rx_buf[SH_RX_BUF_SIZE]; /* 单帧接收缓冲 */
} SensorPort_t;

static SensorPort_t g_ports[SENSOR_HUB_COUNT] = {
    /* sensor 1 (CH1) */ { &huart7,  GPIOA, UART7_RTS_Pin,   0 },   /* UART7  中断 */
    /* sensor 2 (CH2) */ { &huart8,  GPIOE, UART8_RTS_Pin,   0 },   /* UART8  中断 */
    /* sensor 3 (CH3) */ { &huart3,  GPIOE, USART3_RTS_Pin,  1 },   /* USART3 DMA */
    /* sensor 4 (CH4) */ { &huart5,  GPIOE, UART5_RTS_Pin,   1 },   /* UART5  DMA */
    /* sensor 5 (CH5) */ { &huart1,  GPIOD, USART1_RTS_Pin,  1 },   /* USART1 DMA */
    /* sensor 6 (CH6) */ { &huart6,  GPIOG, USART6_RTS_Pin,  1 },   /* USART6 DMA */
    /* sensor 7 (CH7) */ { &huart4,  GPIOA, UART4_RTS_Pin,   1 },   /* UART4  DMA */
    /* sensor 8 (CH8) */ { &huart2,  GPIOD, USART2_RTS_Pin,  1 },   /* USART2 DMA */
};

static QueueHandle_t g_sensor_frame_queue = NULL;

/* ============================ FC06 写应答同步 ============================
 * Modbus 标准：写单保持寄存器(06)的应答 = 请求帧原样回显。
 * SensorHub_WriteReg() 发送前登记期望值并阻塞在信号量上；
 * SensorHub_Task 消费帧队列时先做回显匹配（sh_write_resp_match），
 * 命中后释放信号量唤醒写操作；等待方用超时兜底（传感器离线场景）。
 */
typedef struct {
    volatile uint8_t  armed;      /* 1=正在等待该路应答 */
    volatile uint8_t  matched;    /* 1=回显帧校验通过 */
    uint16_t          reg;        /* 期望寄存器地址 */
    uint16_t          value;      /* 期望写入值 */
    SemaphoreHandle_t sem;
} ShWriteSync_t;

static ShWriteSync_t g_wr_sync[SENSOR_HUB_COUNT];

/* ============================ FC03 读应答同步（写后读回验证用） ============================
 * 与写同步同机制：SensorHub_ReadReg() 发送 FC03 读请求并阻塞在信号量上；
 * SensorHub_Task 消费帧队列时优先匹配 FC03 读应答（sh_read_resp_match），
 * 命中后释放信号量唤醒读操作；超时兜底（传感器离线场景）。
 * 注意：读应答是入队帧，只能由 SensorHub_Task 消费，故复用信号量而非直接 xQueueReceive，
 *       避免与主循环抢帧队列。
 */
typedef struct {
    volatile uint8_t  armed;      /* 1=正在等待该路读应答 */
    volatile uint8_t  matched;    /* 1=读应答帧校验通过 */
    uint16_t          reg;        /* 期望读回的寄存器地址 */
    uint16_t          value;      /* 读回的寄存器值 */
    SemaphoreHandle_t sem;
} ShReadSync_t;

static ShReadSync_t g_rd_sync[SENSOR_HUB_COUNT];

/* ============================ FC04 实时采样同步（单路 sensor_read 用） ============================
 * 网关下发 sensor_read(sensor_id=1..8) 时，先发一次 FC04 查询把缓存 voltage_flag 基线
 * 复位为传感器真实状态，再用更新后的缓存回包。SensorHub_Query() 只发帧不等应答，
 * 应答帧经 ISR -> 帧队列 -> SensorHub_Task -> sensor_cache_update() 更新缓存；本同步量
 * 让 lwIP 线程阻塞等待"该路已收到一帧真实数据"，机制同 FC03 读同步（g_rd_sync）。
 */
typedef struct {
    volatile uint8_t  armed;      /* 1=正在等待该路 FC04 采样应答 */
    SemaphoreHandle_t sem;
} ShSampleSync_t;

static ShSampleSync_t g_sample_sync[SENSOR_HUB_COUNT];

/* ============================ 8 路最新数据暂存（替代边收边报） ============================
 * 收到帧只更新缓存 + 检测 voltage_flag 0→1 边沿，不再边收边报。
 * 上报仅由两条路径触发：
 *   ① 上位机下发 sensor_read 命令 → 回传该路缓存（见 SensorHub_GetCached）；
 *   ② 检测到该路 voltage_flag 由 0→1 突变 → 立即上传该路（仅变化那一路）。
 * temp_flag 变化故意不上传（按需求只盯 voltage_flag）。
 */
typedef struct {
    SensorData_t data;             /* 该路最新一次解析结果 */
    uint8_t      has_data;         /* 是否已收到过有效帧（避免上报全 0 初始值） */
    uint8_t      last_voltage_flag;/* 上一次缓存的 voltage_flag，用于 0→1 边沿检测 */
    uint32_t     last_data_tick;   /* 最近一次收到有效帧时的 HAL_GetTick()（ms），用于 30s 通讯丢失检测 */
    uint32_t     vflag_rise_tick;  /* voltage_flag 由 0→1 时刻的 HAL_GetTick()（ms），用于 15s 持续故障检测 */
    uint8_t      vflag_watch;      /* 1=正处于 voltage_flag 持续监视窗口（尚未回落到 0） */
    uint8_t      vflag_reset_src;  /* 网关读取复位基线时，传感器当时的真实 voltage_flag（0/1）。
                                     * 为 1 = 传感器内部标志位仍锁存为 1（真实状态未清），
                                     * 此时复位基线后紧接着来的同源 1 帧并非新突发，
                                     * 不重复上报（用一次即清 0）。真正的再次突发必然先
                                     * 经历 0 帧，故不会被此逻辑误吞。 */
    uint8_t      boot_fail;        /* 1=上电 FC03 读模式寄存器 0x0007 连续 3 次失败（未收到有效帧前持续 alarm=2，收到帧即清除） */
} sensor_cache_entry_t;

/* 计算某路当前告警类型（调用方须已持有 g_cache_mutex）。
 * 返回并写回 e->data.alarm_type：
 *   0 = 无
 *   1 = 传感器故障（voltage_flag 持续 15s 保持为 1）
 *   2 = 传感器通讯丢失（30s 内无任何数据；含从未收到数据的未接入通道）
 * 注：用 HAL_GetTick()（SysTick ms 计数）做窗口，不受 RTC 慢走影响。 */
static uint8_t sensor_alarm_eval(sensor_cache_entry_t *e)
{
    uint8_t alarm = 0;
    uint32_t now = HAL_GetTick();
    /* 通讯丢失优先（alarm=2）：满足以下任一即判通讯丢失
     *  - mode=1（主动上报）通道距最近一次"通信"已超 30s。主动上报通道本应持续推数据，
     *    静默 30s 即判丢失。mode 由上电 FC03 读 0x0007 探测得到，故此处可靠；
     *    通信 = 有效数据帧或探测应答（两者都会刷新 last_data_tick）。
     *  - 上电 FC03 读模式寄存器连续 3 次失败（boot_fail，收到有效帧即清除）：
     *    未接入 / 不响应的通道，不论 mode 一律直接判丢失。
     * 说明：mode=0（问答）通道由集中器轮询，静默不代表丢失，故不参与 30s 判据。 */
    if ((e->data.mode == SENSOR_MODE_ACTIVE && (now - e->last_data_tick) >= 30000U) ||
        (e->boot_fail && !e->has_data))
    {
        alarm = 2;
    }
    /* 传感器故障：voltage_flag 持续 15s 保持为 1（期间未回落到 0） */
    else if (e->vflag_watch &&
             (now - e->vflag_rise_tick) >= 15000U)
    {
        alarm = 1;
    }

    /* 通讯丢失(alarm=2)：清空缓存的"测量数据"，避免上位机读到过期的旧值。
     * 仅清测量字段 + 复位电压监视窗口；刻意保留以下成员：
     *   - sensor_id ：通道标识，上报必需；
     *   - mode      ：后续恢复后仍需用它做 30s 丢失判据，清零会变 mode=0 退出判据；
     *   - alarm_type：本就是本次清零的触发条件(=2)，不能清；
     *   - last_data_tick / boot_fail / has_data：维持"已丢失"状态，直到新帧到来才清除。
     * 复位 vflag_watch / vflag_rise_tick 可防止旧的 15s 电压窗口在恢复后立刻误触发 alarm=1。 */
    if (alarm == 2)
    {
        e->data.temperature  = 0;
        e->data.voltage_flag = 0;
        e->data.temp_flag    = 0;
        e->vflag_watch       = 0;
        e->vflag_rise_tick   = 0;
    }

    e->data.alarm_type = alarm;
    return alarm;
}

static sensor_cache_entry_t g_sensor_cache[SENSOR_HUB_COUNT];
static SemaphoreHandle_t    g_cache_mutex = NULL;

/* 更新某路缓存并检测 voltage_flag 0→1 边沿触发上报（仅变化那一路） */
static void sensor_cache_update(uint8_t sid, const SensorData_t *sd)
{
    uint8_t trigger = 0;
    SensorData_t snap;             /* 锁外发送的快照，含最新 alarm_type */

    if (g_cache_mutex != NULL)
    {
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    }
    sensor_cache_entry_t *e = &g_sensor_cache[sid - 1];
    uint32_t now = HAL_GetTick();

    /* 边沿判定：基线(last_voltage_flag)为 0 且新值为 1 → 视为新突发。
     * 基线可被 SensorHub_ResetVFlagBase() 强制归零（网关下发读取时复位），
     * 使被"上次触发遗留的 1"卡住的通道重新武装，能再次检出后续突发。
     * 复位后紧接着来的同源 1 帧（传感器内部标志位仍未清）不算新突发，
     * 由 vflag_reset_src 抑制一次，避免把同一次触发重复上报。 */
    if (e->has_data && e->last_voltage_flag == 0 && sd->voltage_flag == 1)
    {
        if (e->vflag_reset_src != 0)
        {
            e->vflag_reset_src = 0;   /* 复位后的首个同源 1：抑制，不上报 */
        }
        else
        {
            trigger = 1;   /* 仅"变化那一路"上传 */
        }
    }

    /* voltage_flag 边沿处理：0→1 启动 15s 持续监视窗口；1→0 关闭窗口 */
    if (e->last_voltage_flag == 0 && sd->voltage_flag == 1)
    {
        e->vflag_rise_tick = now;
        e->vflag_watch = 1;
    }
    else if (sd->voltage_flag == 0)
    {
        e->vflag_watch = 0;
    }

    e->last_data_tick = now;       /* 刷新"最近一次收到数据"时间戳 */
    e->boot_fail = 0;              /* 收到有效帧 = 通讯已恢复，清除上电模式探测失败标记 */
    e->data = *sd;
    e->last_voltage_flag = sd->voltage_flag;
    e->has_data = 1;

    sensor_alarm_eval(e);          /* 计算并写回 e->data.alarm_type */
    snap = e->data;

    if (g_cache_mutex != NULL)
    {
        xSemaphoreGive(g_cache_mutex);
    }
    if (trigger)
    {
        tcp_send_sensor_data(&snap);   /* 锁外发送：避免持锁时进入网络栈引发优先级反转 */
    }
}

/* 复位该路的 voltage_flag 边沿基线（网关下发 sensor_read 时调用）。
 *
 * 背景：传感器为"锁存式"标志位 —— 触发后其内部 voltage_flag 可能一直保持 1，
 *   不会自行清零。若集中器仅以真实读回值更新基线，则该路基线会永久停在 1，
 *   之后每一次真实突发（0→1）都会被边沿检测筛掉，导致多次触发只上报一次。
 *
 * 本函数（方案 A）：
 *   - last_voltage_flag = 0        → 重新武装，该路下次真实突发可被正常检出上报；
 *   - data.voltage_flag 保持真实值   → 上报给网关的值不被篡改，网关能看到传感器实际状态；
 *   - vflag_reset_src 记录复位时传感器的真实状态：若复位时真实值仍为 1，
 *     则复位后紧接着来的同源 1 帧属"同一标志位的延续"，不是新突发，
 *     由 sensor_cache_update() 抑制一次，避免把同一次触发重复上报。
 *
 * 注意：只重置"边沿基线"，不修改 data.voltage_flag / alarm_type / 其它测量值。
 * 返回 0=成功；-1=参数非法。 */
int8_t SensorHub_ResetVFlagBase(uint8_t sid)
{
    if (sid < 1 || sid > SENSOR_HUB_COUNT) return -1;

    if (g_cache_mutex != NULL)
    {
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    }
    sensor_cache_entry_t *e = &g_sensor_cache[sid - 1];

    /* 复位时传感器的真实状态：1 = 内部标志位仍锁存为 1（复位后首个 1 帧属同源延续） */
    e->vflag_reset_src = (e->data.voltage_flag != 0) ? 1U : 0U;
    e->last_voltage_flag = 0;        /* 重新武装：下次真实突发(0→1)可被检出 */

    if (g_cache_mutex != NULL)
    {
        xSemaphoreGive(g_cache_mutex);
    }
    return 0;
}

/* 供上位机 sensor_read 命令读取某路缓存（返回 1=已有有效数据，0=无） */
uint8_t SensorHub_GetCached(uint8_t sid, SensorData_t *out)
{
    if (sid < 1 || sid > SENSOR_HUB_COUNT || out == NULL) return 0;

    uint8_t ok = 0;
    if (g_cache_mutex != NULL)
    {
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    }
    sensor_cache_entry_t *e = &g_sensor_cache[sid - 1];
    sensor_alarm_eval(e);          /* 读取前刷新 alarm_type（如已 30s 无数据→通讯丢失） */
    *out = e->data;
    ok = e->has_data;
    if (g_cache_mutex != NULL)
    {
        xSemaphoreGive(g_cache_mutex);
    }
    return ok;
}

/* 供心跳上报读取 8 路在线状态（1=正常，0=通讯丢失）。
 * 判据与 sensor_alarm_eval 保持一致：alarm_type==2 → 0；其余（含 alarm=1 电压持续故障，
 * 该路通讯仍正常）→ 1。读取前先刷新各路口 alarm_type，故 30s 静默的 mode=1 通道会实时变 0。
 * 一次加锁读回全部，避免心跳里逐路取锁（8 次）。 */
void SensorHub_GetOnlineMask(uint8_t *out, uint8_t count)
{
    if (out == NULL || count == 0) return;
    if (count > SENSOR_HUB_COUNT) count = SENSOR_HUB_COUNT;

    if (g_cache_mutex != NULL)
    {
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    }
    for (uint8_t i = 0; i < count; i++)
    {
        sensor_cache_entry_t *e = &g_sensor_cache[i];
        sensor_alarm_eval(e);                       /* 读取前刷新（如已 30s 无数据 → 通讯丢失） */
        out[i] = (e->data.alarm_type == 2U) ? 0U : 1U;
    }
    for (uint8_t i = count; i < SENSOR_HUB_COUNT; i++)
    {
        out[i] = 0U;                                /* 调用方给的缓冲更大时，其余位置填 0 */
    }
    if (g_cache_mutex != NULL)
    {
        xSemaphoreGive(g_cache_mutex);
    }
}

/* 周期评估全部 8 路告警（在 SensorHub_Task 周期唤醒时调用）。
 * 主要用于检测"mode=1 但 30s 无数据"的通讯丢失——该状态下不会有新帧触发 cache_update。 */
static void SensorHub_EvalAlarms(void)
{
    if (g_cache_mutex != NULL)
    {
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    }
    for (uint8_t i = 0; i < SENSOR_HUB_COUNT; i++)
    {
        sensor_alarm_eval(&g_sensor_cache[i]);
    }
    if (g_cache_mutex != NULL)
    {
        xSemaphoreGive(g_cache_mutex);
    }
}

/* 上电模式探测失败标记（FC03 读 0x0007 连续 3 次失败时调用）。
 * 置 boot_fail 后该路持续 alarm_type=2，直到收到该路有效数据帧自动清除。 */
void SensorHub_MarkBootFail(uint8_t sensor_id)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return;
    if (g_cache_mutex != NULL)
    {
        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    }
    sensor_cache_entry_t *e = &g_sensor_cache[sensor_id - 1];
    e->boot_fail = 1;
    e->data.alarm_type = 2;
    if (g_cache_mutex != NULL)
    {
        xSemaphoreGive(g_cache_mutex);
    }
}

/* 收到完整帧后交给任务解析 */
static void sensorhub_push_frame(uint8_t sensor_id, const uint8_t *data, uint16_t len)
{
    if (g_sensor_frame_queue == NULL) return;
    if (len == 0 || len > SH_RX_BUF_SIZE) return;

    SensorFrame_t f;
    f.sensor_id = sensor_id;
    f.len      = len;
    memcpy(f.data, data, len);

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xQueueSendFromISR(g_sensor_frame_queue, &f, &xHigherPriorityTaskWoken) != pdPASS)
    {
        /* 队列满则丢弃，避免阻塞 ISR */
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* ============================ Modbus CRC16 ============================
 * 多项式 0xA001，初值 0xFFFF。返回值低字节在前（与线序一致）。
 */
static uint16_t modbus_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
        {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

/* ============================ HAL 接收事件回调 ============================
 * 由 HAL_UARTEx_ReceiveToIdle_DMA/IT 在 IDLE 或缓冲满时调用，Size=本次收到字节数。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    for (uint8_t i = 0; i < SENSOR_HUB_COUNT; i++)
    {
        if (g_ports[i].huart == huart)
        {
            if (Size > 0 && Size < SH_RX_BUF_SIZE)
            {
                sensorhub_push_frame(i + 1, g_ports[i].rx_buf, Size);
            }
            /* 重新启动接收 */
            if (g_ports[i].use_dma)
            {
                HAL_UARTEx_ReceiveToIdle_DMA(huart, g_ports[i].rx_buf, SH_RX_BUF_SIZE);
            }
            else
            {
                HAL_UARTEx_ReceiveToIdle_IT(huart, g_ports[i].rx_buf, SH_RX_BUF_SIZE);
            }
            break;
        }
    }
}

/* 前向声明：供下方的 HAL_UART_ErrorCallback 调用 */
static void sh_start_rx(uint8_t idx);

/* ============================ HAL 错误回调 ============================
 * ORE / FE / NE / PE / DMA 错误都会走到这里。HAL 库自带的是 weak 空实现，
 * 不实现本函数的话，一次错误就会让该通道永久停止接收（关中断 + 停 DMA）。
 * 这里统一：清标志 -> 复位 HAL 状态 -> 重启接收，让通道自愈。
 * 注意：本回调在中断上下文执行，只做非阻塞操作。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == NULL) return;

    for (uint8_t i = 0; i < SENSOR_HUB_COUNT; i++)
    {
        if (g_ports[i].huart != huart) continue;

#if SH_DEBUG_UART_ID
        /* 调试打印口不参与接收，忽略 */
        if ((i + 1) == (uint8_t)SH_DEBUG_UART_ID) return;
#endif

        /* DMA 通道需先停掉 DMA，否则重新启动会被判 BUSY */
        if (g_ports[i].use_dma && huart->hdmarx != NULL)
        {
            HAL_UART_DMAStop(huart);
        }

        sh_start_rx(i);
        return;
    }
}

/* ============================ UART 错误恢复 ============================
 * 背景（典型故障）：上电前该路已接好并通电的传感器，上电后这一路收不到数据、
 * 也不再进中断。原因是从 MX_USARTx_UART_Init()（此时 RX 已使能、但没人读 RDR）
 * 到 SensorHub_Init() 启动接收之间有几百毫秒~数秒，期间总线上来的数据无人读取
 * -> 溢出 ORE 置位（噪声还会带 FE/NE）。一旦随后使能 RXNE/IDLE 中断，ORE 立刻
 * 触发中断并走 HAL 的 error 分支；工程里没有 HAL_UART_ErrorCallback 强实现，
 * HAL 会把 RxState 复位、关接收中断、停 DMA -> 该通道永久失效。
 * 对策：① 每次启动接收前先冲刷 RDR 并清掉全部错误标志；
 *       ② 实现 HAL_UART_ErrorCallback，出错后自愈重启接收（运行期干扰同样受益）。
 */
static void sh_uart_flush_rx(UART_HandleTypeDef *huart)
{
    if (huart == NULL || huart->Instance == NULL) return;

    /* 1. 读掉 RDR 里的残留字节（清 RXNE） */
    if ((huart->Instance->ISR & USART_ISR_RXNE) != 0U)
    {
        (void)huart->Instance->RDR;
    }
    /* 2. 冲刷接收数据寄存器（再清一次 RXNE 并丢弃残留） */
    SET_BIT(huart->Instance->RQR, USART_RQR_RXFRQ);
    /* 3. 清全部接收错误标志：PE / FE / NE / ORE / IDLE */
    WRITE_REG(huart->Instance->ICR,
              USART_ICR_PECF | USART_ICR_FECF | USART_ICR_NCF |
              USART_ICR_ORECF | USART_ICR_IDLECF);
    /* 4. 复位 HAL 侧错误状态，否则下次启动接收会被判 BUSY / 带错 */
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    huart->RxState   = HAL_UART_STATE_READY;
}

/* 启动某一路的接收（先冲刷清错，再按 DMA / 中断方式启动） */
static void sh_start_rx(uint8_t idx)
{
    UART_HandleTypeDef *huart = g_ports[idx].huart;

    sh_uart_flush_rx(huart);

    if (g_ports[idx].use_dma && huart->hdmarx != NULL)
    {
        /* RX DMA 由 CubeMX 的 CIRCULAR 改为 NORMAL，配合 ReceiveToIdle
         * 每次 IDLE 后重新启动，避免环形计数越界问题。 */
        huart->hdmarx->Init.Mode = DMA_NORMAL;
        HAL_DMA_Init(huart->hdmarx);

        HAL_UARTEx_ReceiveToIdle_DMA(huart, g_ports[idx].rx_buf, SH_RX_BUF_SIZE);
    }
    else
    {
        HAL_UARTEx_ReceiveToIdle_IT(huart, g_ports[idx].rx_buf, SH_RX_BUF_SIZE);
    }
}

/* ============================ 初始化 ============================ */
void SensorHub_Init(void)
{
    g_sensor_frame_queue = xQueueCreate(16, sizeof(SensorFrame_t));

    for (uint8_t i = 0; i < SENSOR_HUB_COUNT; i++)
    {
        /* 各路写应答同步信号量（写模式等 FC06 操作依赖，含调试口占用路也创建，
         * 写入时会被 SensorHub_WriteReg 的调试口检查拦下，不占用资源） */
        g_wr_sync[i].armed   = 0;
        g_wr_sync[i].matched = 0;
        if (g_wr_sync[i].sem == NULL)
        {
            g_wr_sync[i].sem = xSemaphoreCreateBinary();
        }

        g_rd_sync[i].armed   = 0;
        g_rd_sync[i].matched = 0;
        if (g_rd_sync[i].sem == NULL)
        {
            g_rd_sync[i].sem = xSemaphoreCreateBinary();
        }

        /* FC04 实时采样同步信号量（单路 sensor_read 复位 voltage_flag 基线用） */
        g_sample_sync[i].armed = 0;
        if (g_sample_sync[i].sem == NULL)
        {
            g_sample_sync[i].sem = xSemaphoreCreateBinary();
        }

        /* 缓存互斥量只创建一次（循环里幂等判断） */
        if (g_cache_mutex == NULL)
        {
            g_cache_mutex = xSemaphoreCreateMutex();
        }

        /* 上电默认各通道工作模式 = 1（主动上报）。之后以实际读到的为准：
         *   - 上电 FC03 读 0x0007 探测成功 → 覆盖为该路真实 mode（SensorHub_BootProbeModes）
         *   - 网关 mode_set_req → 覆盖为下发值
         * 好处：探测完成前，"mode=1 且 30s 无数据 → alarm_type=2"的判据即已生效，
         *       未接/无应答通道不会因为 mode 默认为 0 而漏报。 */
        g_sensor_cache[i].data.mode = SENSOR_MODE_ACTIVE;

#if SH_DEBUG_UART_ID
        /* 该路已被临时占用为调试打印口：不启动接收，避免自发自收与误解析 */
        if ((i + 1) == (uint8_t)SH_DEBUG_UART_ID) continue;
#endif

        /* 方向引脚默认置低 = 接收态 */
        HAL_GPIO_WritePin(g_ports[i].de_port, g_ports[i].de_pin, GPIO_PIN_RESET);

        /* 启动接收（内部会先冲刷 RDR 并清 ORE/FE/NE/PE，避免上电窗口期的
         * 残留错误标志一使能中断就把本通道打死） */
        sh_start_rx(i);
    }

#if SH_DEBUG_UART_ID
    /* 初始化临时调试口（内部防重入）；必须在跳过该路接收之后 */
    DebugUart_Init();
#endif
}

/* ============================ 硬件信息查询 ============================
 * 供 debug_uart 等模块复用端口映射表，避免重复维护 UART/DE 引脚。
 */
void SensorHub_GetPortHw(uint8_t sensor_id,
                         UART_HandleTypeDef **huart,
                         GPIO_TypeDef **de_port,
                         uint16_t *de_pin)
{
    *huart   = NULL;
    *de_port = NULL;
    *de_pin  = 0;

    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return;

    *huart   = g_ports[sensor_id - 1].huart;
    *de_port = g_ports[sensor_id - 1].de_port;
    *de_pin  = g_ports[sensor_id - 1].de_pin;
}

#if !SH_LOOPBACK_ECHO
/* ============================ 帧解析 ============================
 * 类 Modbus 读应答帧：addr(1) func=0x04(1) byte_count(1) data(N) crc(2)
 * 数据寄存器从 0x0000 起连续排列（按 SH_REG_* 布局）。
 */
static uint8_t sensorhub_parse_frame(uint8_t sensor_id, const uint8_t *buf, uint16_t len, SensorData_t *sd)
{
    if (len < 5) return 0;                       /* 最小帧：addr func cnt + crc */
    if (buf[0] != SENSOR_HUB_ADDR) return 0;     /* 从机地址校验 */
    if (buf[1] != SH_FUNC_READ)   return 0;      /* 仅处理 04 读应答 */

    uint8_t  byte_count = buf[2];
    uint16_t data_len   = byte_count;
    if (len != (uint16_t)(3 + data_len + 2)) return 0; /* 长度校验 */

    /* CRC 校验（覆盖除 CRC 外的全部字节） */
    uint16_t crc_calc = modbus_crc16(buf, len - 2);
    uint16_t crc_recv = (uint16_t)((buf[len - 1] << 8) | buf[len - 2]);
    if (crc_calc != crc_recv) return 0;

    /* 逐寄存器解析（假设数据从 0x0000 起连续） */
    memset(sd, 0, sizeof(*sd));
    sd->sensor_id = sensor_id;

    uint16_t nregs = data_len / 2;
    uint16_t pos   = 3;

    for (uint16_t i = 0; i < nregs; i++)
    {
        uint16_t r = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        pos += 2;

        if (i == SH_REG_VOLTAGE_FLAG)
        {
            sd->voltage_flag = (r != 0) ? 1 : 0;
        }
        else if (i == SH_REG_TEMP_FLAG)
        {
            sd->temp_flag = (r != 0) ? 1 : 0;
        }
        else if (i == SH_REG_TEMPERATURE + 1)
        {
            /* 温度 float32：本传感器按 4 字节小端（IEEE754）存放于 reg[2]~reg[3]，
               frame 内字节顺序 reg2_lo, reg2_hi, reg3_lo, reg3_hi 即 float 的小端内存映像。
               直接按该 4 字节小端拼成 uint32 再转 float，避免高/低 16 位拼反。 */
            const uint8_t *tp = &buf[3 + (uint16_t)SH_REG_TEMPERATURE * 2];
            uint32_t raw = ((uint32_t)tp[0])       | ((uint32_t)tp[1] << 8) |
                           ((uint32_t)tp[2] << 16) | ((uint32_t)tp[3] << 24);
            float t = *((const float *)&raw);
            sd->temperature = (int32_t)(t * 100.0f);   /* 放大 100 倍，2575 = 25.75℃ */
        }
        /* 寄存器 4..5 不存在（无电压寄存器），忽略 */
    }

    /* 电压：传感器无该寄存器，协议 sensor_data 不再携带 voltage 字段；
       若需真实电压，应另接 ADC 采样输入，不再通过 RS485 帧。 */
    sd->mode = sensorhub_get_mode(sensor_id);

    return 1;
}
#endif /* !SH_LOOPBACK_ECHO */

/* ============================ FC06 回显应答匹配 ============================
 * 在采集任务消费帧队列时调用（先于数据帧解析）。
 * 仅当该路有未完成的写等待(armed)时尝试匹配：
 *   标准回显帧 = addr(01) func(06) reg_hi reg_lo val_hi val_lo crc_lo crc_hi，8 字节。
 *   实测某些从机回显尾部会多带冗余字节（如 01 06 00 07 00 01 F9 CB 00，9 字节），
 *   故只校验前 8 字节（内容 + CRC），帧长 8~16 字节均接受，尾部忽略。
 * 命中 -> 清 armed、按 CRC 结果置 matched、释放信号量唤醒写方，帧已消费返回 1；
 * 其它帧（如主动上报 FC04）不拦截不动状态，返回 0 继续走正常解析。
 */
#define SH_WRITE_RESP_MIN_LEN   8U
#define SH_WRITE_RESP_MAX_LEN   16U

static uint8_t sh_write_resp_match(uint8_t sensor_id, const uint8_t *buf, uint16_t len)
{
    ShWriteSync_t *w = &g_wr_sync[sensor_id - 1];
    if (!w->armed) return 0;

    if (len >= SH_WRITE_RESP_MIN_LEN && len <= SH_WRITE_RESP_MAX_LEN &&
        buf[0] == SENSOR_HUB_ADDR &&
        buf[1] == SH_FUNC_WRITE &&
        buf[2] == (uint8_t)(w->reg >> 8) &&
        buf[3] == (uint8_t)(w->reg & 0xFFU) &&
        buf[4] == (uint8_t)(w->value >> 8) &&
        buf[5] == (uint8_t)(w->value & 0xFFU))
    {
        uint16_t crc_calc = modbus_crc16(buf, 6);
        uint16_t crc_recv = (uint16_t)((buf[7] << 8) | buf[6]);

        w->armed   = 0;
        w->matched = (crc_calc == crc_recv) ? 1U : 0U;
        if (w->sem != NULL)
        {
            xSemaphoreGive(w->sem);
        }
        return 1;
    }
    return 0;
}


#if !SH_LOOPBACK_ECHO
/* ============================ 上电模式探测（FC03 读 0x0007） ============================
 * 每路发送 FC03 读保持寄存器 0x0007（Modbus/主动上报版本标志位），重试 3 次；
 * 成功 → 回填 g_sensor_mode（sensorhub_set_mode）与该路缓存 data.mode；
 * 3 次全部失败 → SensorHub_MarkBootFail() 置 alarm_type=2（收到有效帧后自动消除）。
 * 等待应答期间收到的其它帧照常解析入库，主动上报通道不丢数据。
 * 注意：本函数在 SensorHub_Task 主循环之前运行（该任务是帧队列唯一消费者，
 *       应答匹配直接在此消费队列完成，不能像 FC06 那样跨任务阻塞等信号量）。
 * 耗时上限：8 路 × 3 次 × 500ms ≈ 12s（全离线最坏情况）。
 */
static uint8_t sh_fc03_resp_match(const uint8_t *buf, uint16_t len, uint16_t *out_val)
{
    /* 应答帧：addr(01) func(03) bytecnt(02) val_hi val_lo crc16 = 7 字节 */
    if (len != 7) return 0;
    if (buf[0] != SENSOR_HUB_ADDR || buf[1] != SH_FUNC_READ_HOLDING) return 0;
    if (buf[2] != 0x02) return 0;
    uint16_t crc_calc = modbus_crc16(buf, 5);
    uint16_t crc_recv = (uint16_t)((buf[6] << 8) | buf[5]);
    if (crc_calc != crc_recv) return 0;
    *out_val = (uint16_t)((buf[3] << 8) | buf[4]);
    return 1;
}

/* ============================ 写后读回（FC03 读 0x0007 验证） ============================
 * 设置某路 mode（FC06 写）后，再用 FC03 读回该路实际状态，以真实值更新全局
 * 缓存（g_sensor_mode[] 与 g_sensor_cache[].data.mode），避免“写回显通过但传感器
 * 实际未生效”导致全局与实际不一致。
 *   - sh_read_resp_match：在 SensorHub_Task 收帧处（紧接 sh_write_resp_match）调用，
 *     匹配本路对本 reg 的 FC03 读应答并释放读信号量；
 *   - SensorHub_ReadReg：发送 FC03 并阻塞等读信号量，返回寄存器值；
 *   - SensorHub_ReadModeBack：读回 + 更新全局，供 sensorhub_write_mode 调用。
 */

/* 读应答匹配：与 sh_write_resp_match 同机制。仅当该路读 armed 时拦截本路对本 reg 的
 * FC03 应答帧（帧格式/CRC 由 sh_fc03_resp_match 校验），命中则取值并释放信号量。 */
static uint8_t sh_read_resp_match(uint8_t sensor_id, const uint8_t *buf, uint16_t len)
{
    ShReadSync_t *r = &g_rd_sync[sensor_id - 1];
    if (!r->armed) return 0;

    uint16_t out_val = 0;
    if (sh_fc03_resp_match(buf, len, &out_val))
    {
        r->value   = out_val;
        r->armed   = 0;
        r->matched = 1;
        if (r->sem != NULL)
        {
            xSemaphoreGive(r->sem);
        }
        return 1;
    }
    return 0;
}

/* 读单个保持寄存器（功能码 03）：发送后阻塞等待读应答并校验，返回寄存器值。
 * 返回 0=成功(*out_val 有效)；-1=参数/资源/发送错；-2=该路被调试口占用；
 *      -3=应答超时（离线）；-4=应答校验失败。 */
int8_t SensorHub_ReadReg(uint8_t sensor_id, uint16_t reg, uint16_t *out_val)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return -1;
    if (out_val == NULL) return -1;
#if SH_DEBUG_UART_ID
    if (sensor_id == (uint8_t)SH_DEBUG_UART_ID) return -2;  /* 该路已被调试口占用 */
#endif
    SensorPort_t  *p = &g_ports[sensor_id - 1];
    ShReadSync_t  *r = &g_rd_sync[sensor_id - 1];
    if (r->sem == NULL) return -1;

    uint8_t req[8];
    req[0] = SENSOR_HUB_ADDR;
    req[1] = SH_FUNC_READ_HOLDING;
    req[2] = (reg >> 8) & 0xFF;
    req[3] = reg & 0xFF;
    req[4] = 0x00;                                     /* 寄存器数量高字节 */
    req[5] = 0x01;                                     /* 寄存器数量低字节：读 1 个 */
    uint16_t crc = modbus_crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    int8_t ret = -3;
    for (uint8_t attempt = 0; attempt < (uint8_t)SH_MODE_READBACK_RETRY; attempt++)
    {
        r->reg     = reg;
        r->value   = 0;
        r->matched = 0;
        r->armed   = 1;
        (void)xSemaphoreTake(r->sem, 0);                   /* 清残留计数 */

        HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_SET);
        HAL_StatusTypeDef st = HAL_UART_Transmit(p->huart, req, 8, 100);
        HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_RESET);

        if (st != HAL_OK)
        {
            r->armed = 0;
            ret = -1;                                  /* 发送失败：线路/从机异常，不再重试 */
            break;
        }

        if (xSemaphoreTake(r->sem, pdMS_TO_TICKS(SH_MODE_RESP_TIMEOUT_MS)) == pdPASS)
        {
            r->armed = 0;
            ret = r->matched ? (int8_t)0 : (int8_t)-4;
        }
        else
        {
            r->armed = 0;
            ret = -3;                                  /* 应答超时 -> 离线 */
        }

        if (ret == 0) break;                           /* 读回成功，退出重试 */

        /* 超时(-3)或 CRC 错(-4)：短暂间隔后重发 FC03 */
        if (attempt + 1 < (uint8_t)SH_MODE_READBACK_RETRY)
        {
            vTaskDelay(pdMS_TO_TICKS(SH_MODE_READBACK_GAP_MS));
        }
    }

    if (ret == 0 && out_val != NULL) *out_val = r->value;
    return ret;
}

/* 写 mode 后读回该路真实状态并更新全局：
 *   - g_sensor_mode[]（mode 全局，net_comm_task.c 持久化/上报引用）；
 *   - g_sensor_cache[sensor_id-1].data.mode（上报用缓存，保证网关看到真实值）。
 * 返回 0=读回成功并更新；<0=读回失败（见 SensorHub_ReadReg 返回值，失败时不改全局，
 *       由调用方用期望值兜底）。 */
int8_t SensorHub_ReadModeBack(uint8_t sensor_id, uint8_t *out_mode)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return -1;
    if (out_mode == NULL) return -1;

    /* 写后延时：从机改 mode 需一点处理时间，立即读会被丢弃导致超时(-3) */
    vTaskDelay(pdMS_TO_TICKS(SH_MODE_READBACK_DELAY_MS));

    uint16_t val = 0;
    int8_t rd = SensorHub_ReadReg(sensor_id, SH_REG_MODE, &val);
    if (rd != 0)
    {
        *out_mode = SENSOR_MODE_REG;
        return rd;
    }

    uint8_t m = (val == SENSOR_MODE_ACTIVE) ? SENSOR_MODE_ACTIVE : SENSOR_MODE_REG;
    sensorhub_set_mode(sensor_id, m);                  /* 更新 g_sensor_mode[] */
    if (g_cache_mutex != NULL) xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
    g_sensor_cache[sensor_id - 1].data.mode = m;      /* 更新上报缓存 */
    if (g_cache_mutex != NULL) xSemaphoreGive(g_cache_mutex);

    *out_mode = m;
    return 0;
}

static void SensorHub_BootProbeModes(void)
{
    SensorFrame_t f;

    for (uint8_t sid = 1; sid <= SENSOR_HUB_COUNT; sid++)
    {
#if SH_DEBUG_UART_ID
        if (sid == (uint8_t)SH_DEBUG_UART_ID)
        {
            LOG_SH("S%u boot probe skip (debug port)\r\n", (unsigned)sid);
            continue;
        }
#endif
        uint8_t ok = 0;

        for (uint8_t attempt = 0; attempt < 3 && !ok; attempt++)
        {
            SensorPort_t *p = &g_ports[sid - 1];

            uint8_t req[8];
            req[0] = SENSOR_HUB_ADDR;
            req[1] = SH_FUNC_READ_HOLDING;
            req[2] = (SH_REG_MODE >> 8) & 0xFF;
            req[3] = SH_REG_MODE & 0xFF;
            req[4] = 0x00;                          /* 寄存器数量高字节 */
            req[5] = 0x01;                          /* 寄存器数量低字节：读 1 个 */
            uint16_t crc = modbus_crc16(req, 6);
            req[6] = crc & 0xFF;
            req[7] = (crc >> 8) & 0xFF;

            HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_SET);
            HAL_StatusTypeDef st = HAL_UART_Transmit(p->huart, req, 8, 100);
            HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_RESET);
            if (st != HAL_OK) continue;             /* 发送失败计一次失败，重试 */

            /* 阻塞等待应答（SH_MODE_RESP_TIMEOUT_MS）：期间其它帧照常处理 */
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SH_MODE_RESP_TIMEOUT_MS);
            for (;;)
            {
                TickType_t now_t = xTaskGetTickCount();
                TickType_t remain = (deadline > now_t) ? (deadline - now_t) : 0;
                if (xQueueReceive(g_sensor_frame_queue, &f, remain) != pdPASS)
                {
                    break;                          /* 超时 → 重试 */
                }

                uint16_t mode_val = 0;
                if (f.sensor_id == sid && sh_fc03_resp_match(f.data, f.len, &mode_val))
                {
                    uint8_t m = (mode_val == SENSOR_MODE_ACTIVE) ? SENSOR_MODE_ACTIVE : SENSOR_MODE_REG;
                    sensorhub_set_mode(sid, m);     /* 回填真实模式缓存（net_comm_task.c） */
                    if (g_cache_mutex != NULL)
                    {
                        xSemaphoreTake(g_cache_mutex, portMAX_DELAY);
                    }
                    g_sensor_cache[sid - 1].data.mode = m;   /* 缓存无数据时上报也能带真实 mode */
                    g_sensor_cache[sid - 1].boot_fail = 0;
                    g_sensor_cache[sid - 1].last_data_tick = HAL_GetTick(); /* 探测应答计作一次通信，重置 30s 丢失窗口 */
                    if (g_cache_mutex != NULL)
                    {
                        xSemaphoreGive(g_cache_mutex);
                    }
                    LOG_SH("S%u boot mode=%u\r\n", (unsigned)sid, (unsigned)m);
                    ok = 1;
                    break;
                }

                /* 非本路 FC03 应答：FC06 回显拦截 / FC04 数据帧照常解析入库 */
                SensorData_t sd;
                if (sh_write_resp_match(f.sensor_id, f.data, f.len))
                {
                    continue;
                }
                if (sensorhub_parse_frame(f.sensor_id, f.data, f.len, &sd))
                {
                    sensor_cache_update(f.sensor_id, &sd);
                }
            }
        }

        if (!ok)
        {
            LOG_SH("S%u boot mode read FAIL x3 -> alarm=2\r\n", (unsigned)sid);
            SensorHub_MarkBootFail(sid);
        }
    }
}
#endif /* !SH_LOOPBACK_ECHO */

/* ============================ 采集任务 ============================ */
void SensorHub_Task(const void *argument)
{
    SensorHub_Init();

#if !SH_LOOPBACK_ECHO
    /* 上电探测 8 路工作模式（FC03 读保持寄存器 0x0007，每路重试 3 次）。
     * 成功 → 回填该路 mode 缓存与 data.mode；3 次全失败 → 该路直接 alarm_type=2。
     * 必须放在主循环之前：本函数是帧队列的唯一消费者，探测期间收到的其它帧由其直接解析入库。 */
    SensorHub_BootProbeModes();
#endif

#if SH_LOOPBACK_ECHO
#if SH_DEBUG_UART_ID
    printf("[LB] Echo-loopback test ON. Send data to each port; S%u is the console (not echoed).\r\n",
           (unsigned)SH_DEBUG_UART_ID);
#endif
#endif

    SensorFrame_t f;
    for (;;)
    {
        /* 1000ms 超时唤醒：即使无新帧也定期评估告警（如 30s 无数据→通讯丢失） */
        if (xQueueReceive(g_sensor_frame_queue, &f, pdMS_TO_TICKS(1000)) == pdPASS)
        {
#if SH_LOOPBACK_ECHO
            /* ---- PC 回显测试模式 ----
             * 把每路（非调试口）收到的数据原样发回，验证 RX + TX + 方向脚(DE/RE)。
             * 不解析、不上报 TCP。用户用 USB-RS485 转接板连各路发数据看回显即可。 */
            if ((SH_DEBUG_UART_ID == 0) || (f.sensor_id != (uint8_t)SH_DEBUG_UART_ID))
            {
                SensorPort_t *p = &g_ports[f.sensor_id - 1];
                printf("[ECHO S%u] ", (unsigned)f.sensor_id);
                for (uint16_t k = 0; k < f.len; k++)
                {
                    printf("%02X ", f.data[k]);
                }
                printf("\r\n");

                HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_SET);
                HAL_UART_Transmit(p->huart, f.data, f.len, 100);
                HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_RESET);
            }
            /* echo 模式下不解析、不上报 TCP，避免噪声 */
#else
            SensorData_t sd;

#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
            /* 打印该路收到的原始帧（含 CRC），便于核对协议 */
            LOG_SH("S%u RX :", (unsigned)f.sensor_id);
            for (uint16_t k = 0; k < f.len; k++)
            {
                printf("%02X ", f.data[k]);
            }
            printf("\r\n");
#endif

            /* FC06 写应答回显拦截：写等待期间优先匹配，命中则不再走数据解析 */
            if (sh_write_resp_match(f.sensor_id, f.data, f.len))
            {
#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
                LOG_SH("S%u FC06 echo matched\r\n", (unsigned)f.sensor_id);
#endif
                continue;
            }

            /* FC03 读应答匹配（写后读回验证）：armed 时拦截本路读应答，释放读信号量 */
            if (sh_read_resp_match(f.sensor_id, f.data, f.len))
            {
#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
                LOG_SH("S%u FC03 read matched\r\n", (unsigned)f.sensor_id);
#endif
                continue;
            }

            if (sensorhub_parse_frame(f.sensor_id, f.data, f.len, &sd))
            {
#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
                LOG_SH("temp=%ld vflag=%u tflag=%u mode=%u\r\n",
                       (long)sd.temperature,
                       (unsigned)sd.voltage_flag,
                       (unsigned)sd.temp_flag,
                       (unsigned)sd.mode);
#endif
                sensor_cache_update(f.sensor_id, &sd);

                /* FC04 实时采样应答已解析入缓存 → 唤醒等待中的 sensor_read(lwIP线程)。
                 * 必须在 sensor_cache_update() 之后 give，确保调用方读回的是刚更新的真实值。 */
                if (g_sample_sync[f.sensor_id - 1].armed)
                {
                    g_sample_sync[f.sensor_id - 1].armed = 0;
                    if (g_sample_sync[f.sensor_id - 1].sem != NULL)
                    {
                        xSemaphoreGive(g_sample_sync[f.sensor_id - 1].sem);
                    }
                }
            }
#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
            else
            {
                LOG_SH("parse FAIL\r\n");
            }
#endif
#endif
        }
        else
        {
            SensorHub_EvalAlarms();   /* 队列超时（无新帧）：周期评估 8 路告警 */
        }
    }
}

/* ============================ 主动查询 ============================
 * 功能码 04：addr=01 func=04 start=0x0000 count=SH_UPLOAD_REG_COUNT crc16
 */
int8_t SensorHub_Query(uint8_t sensor_id)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return -1;
#if SH_DEBUG_UART_ID
    if (sensor_id == (uint8_t)SH_DEBUG_UART_ID) return -2;  /* 该路已被调试口占用 */
#endif
    SensorPort_t *p = &g_ports[sensor_id - 1];

    uint8_t req[8];
    req[0] = SENSOR_HUB_ADDR;
    req[1] = SH_FUNC_READ;
    req[2] = 0x00;                                     /* 起始寄存器高字节 */
    req[3] = 0x00;                                     /* 起始寄存器低字节 */
    req[4] = (SH_UPLOAD_REG_COUNT >> 8) & 0xFF;        /* 寄存器数量高字节 */
    req[5] = SH_UPLOAD_REG_COUNT & 0xFF;               /* 寄存器数量低字节 */
    uint16_t crc = modbus_crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    /* RS485 切发送 → 发送 → 切回接收 */
    HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_SET);
    HAL_StatusTypeDef st = HAL_UART_Transmit(p->huart, req, 8, 100);
    HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_RESET);

    return (st == HAL_OK) ? 0 : -1;
}

/* 单路实时采样：发 FC04 查询并阻塞等待该路应答帧解析入缓存（复位 voltage_flag 基线）。
 * 供 sensor_read(sensor_id=1..8) 在回包前调用，确保回传的是刚读回的传感器真实状态；
 * 这样下次真实触发时 voltage_flag 能从复位后的 0 再变 1，被边沿检测正常上报。
 * 返回 0=已收到应答/缓存已更新；-1=参数/资源错；-2=该路被调试口占用；-3=应答超时(离线)。 */
int8_t SensorHub_SampleNow(uint8_t sensor_id)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return -1;
#if SH_DEBUG_UART_ID
    if (sensor_id == (uint8_t)SH_DEBUG_UART_ID) return -2;  /* 调试口占用，无法真实采样 */
#endif
    ShSampleSync_t *s = &g_sample_sync[sensor_id - 1];
    if (s->sem == NULL) return -1;

    /* 登记等待并清残留计数，必须在发帧之前（应答随即经 ISR -> 帧队列到达） */
    s->armed = 1;
    (void)xSemaphoreTake(s->sem, 0);

    int8_t qret = SensorHub_Query(sensor_id);   /* 发 FC04 查询帧 */
    if (qret != 0)
    {
        s->armed = 0;
        return -1;                              /* 发送失败 */
    }

    int8_t ret;
    if (xSemaphoreTake(s->sem, pdMS_TO_TICKS(SH_MODE_RESP_TIMEOUT_MS)) == pdPASS)
    {
        ret = 0;                                /* 应答已解析入缓存 */
    }
    else
    {
        ret = -3;                               /* 应答超时（离线/不应答查询） */
    }
    s->armed = 0;
    return ret;
}

/* ============================ 写保持寄存器（功能码 06） ============================
 * 发送 addr=01 func=06 reg_hi reg_lo val_hi val_lo crc16（8 字节），
 * 然后阻塞等待从机回显应答（由 SensorHub_Task 的 sh_write_resp_match 匹配释放），
 * 超时 SH_MODE_RESP_TIMEOUT_MS 仍无应答判定传感器离线。
 * 返回 0 = 应答校验通过；-1 = 参数/资源错误；-2 = 该路被调试口占用；
 *      -3 = 应答超时（离线）；-4 = 应答内容/CRC 校验失败。
 */
int8_t SensorHub_WriteReg(uint8_t sensor_id, uint16_t reg, uint16_t value)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return -1;
#if SH_DEBUG_UART_ID
    if (sensor_id == (uint8_t)SH_DEBUG_UART_ID) return -2;  /* 该路已被调试口占用 */
#endif
    SensorPort_t  *p = &g_ports[sensor_id - 1];
    ShWriteSync_t *w = &g_wr_sync[sensor_id - 1];
    if (w->sem == NULL) return -1;

    uint8_t req[8];
    req[0] = SENSOR_HUB_ADDR;
    req[1] = SH_FUNC_WRITE;
    req[2] = (reg >> 8) & 0xFF;
    req[3] = reg & 0xFF;
    req[4] = (value >> 8) & 0xFF;
    req[5] = value & 0xFF;
    uint16_t crc = modbus_crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    /* 登记期望应答必须在发送之前（应答随即就会经 ISR -> 帧队列到达），
     * 并清掉上次残留的信号量计数 */
    w->reg     = reg;
    w->value   = value;
    w->matched = 0;
    w->armed   = 1;
    (void)xSemaphoreTake(w->sem, 0);

    /* RS485 切发送 -> 发送 -> 切回接收 */
    HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_SET);
    HAL_StatusTypeDef st = HAL_UART_Transmit(p->huart, req, 8, 100);
    HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_RESET);

    int8_t ret;
    if (st != HAL_OK)
    {
        w->armed = 0;
        ret = -1;                                   /* UART 发送失败 */
    }
    else if (xSemaphoreTake(w->sem, pdMS_TO_TICKS(SH_MODE_RESP_TIMEOUT_MS)) == pdPASS)
    {
        w->armed = 0;
        ret = w->matched ? (int8_t)0 : (int8_t)-4;  /* -4 = 回显内容/CRC 不符 */
    }
    else
    {
        w->armed = 0;
        ret = -3;                                   /* 应答超时 -> 传感器离线 */
    }
    return ret;
}
