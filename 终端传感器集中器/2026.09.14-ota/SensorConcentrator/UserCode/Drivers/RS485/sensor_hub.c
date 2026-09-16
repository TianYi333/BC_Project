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
    uint16_t temp_hi = 0;

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
        else if (i == SH_REG_TEMPERATURE)
        {
            temp_hi = r;
        }
        else if (i == SH_REG_TEMPERATURE + 1)
        {
            uint32_t raw = ((uint32_t)temp_hi << 16) | r;
            float t = *((float *)&raw);
            sd->temperature = (int32_t)(t * 100.0f);   /* 放大 100 倍，2550 = 25.50℃ */
        }
        /* 寄存器 4..5 不存在（无电压寄存器），忽略 */
    }

    /* 电压：传感器无该寄存器，sd->voltage 已被 memset 清 0；
       若需真实电压，应另接 ADC 采样输入，不再通过 RS485 帧。 */
    sd->mode = sensorhub_get_mode(sensor_id);

    return 1;
}
#endif /* !SH_LOOPBACK_ECHO */

/* ============================ 采集任务 ============================ */
void SensorHub_Task(const void *argument)
{
    SensorHub_Init();

#if SH_LOOPBACK_ECHO
#if SH_DEBUG_UART_ID
    printf("[LB] Echo-loopback test ON. Send data to each port; S%u is the console (not echoed).\r\n",
           (unsigned)SH_DEBUG_UART_ID);
#endif
#endif

    SensorFrame_t f;
    for (;;)
    {
        if (xQueueReceive(g_sensor_frame_queue, &f, portMAX_DELAY) == pdPASS)
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

            if (sensorhub_parse_frame(f.sensor_id, f.data, f.len, &sd))
            {
#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
                LOG_SH("temp=%ld vflag=%u tflag=%u mode=%u\r\n",
                       (long)sd.temperature,
                       (unsigned)sd.voltage_flag,
                       (unsigned)sd.temp_flag,
                       (unsigned)sd.mode);
#endif
                tcp_send_sensor_data(&sd);
            }
#if (SH_DEBUG_UART_ID && SH_DEBUG_DUMP_FRAME)
            else
            {
                LOG_SH("parse FAIL\r\n");
            }
#endif
#endif
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

/* ============================ 写保持寄存器（功能码 06，预留） ============================
 * addr=01 func=06 reg_hi reg_lo val_hi val_lo crc16
 */
int8_t SensorHub_WriteReg(uint8_t sensor_id, uint16_t reg, uint16_t value)
{
    if (sensor_id < 1 || sensor_id > SENSOR_HUB_COUNT) return -1;
#if SH_DEBUG_UART_ID
    if (sensor_id == (uint8_t)SH_DEBUG_UART_ID) return -2;  /* 该路已被调试口占用 */
#endif
    SensorPort_t *p = &g_ports[sensor_id - 1];

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

    HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_SET);
    HAL_StatusTypeDef st = HAL_UART_Transmit(p->huart, req, 8, 100);
    HAL_GPIO_WritePin(p->de_port, p->de_pin, GPIO_PIN_RESET);

    return (st == HAL_OK) ? 0 : -1;
}
