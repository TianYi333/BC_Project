/**
  ******************************************************************************
  * @file    sensor_hub.h
  * @brief   8 路 RS485 终端传感器采集层（类 Modbus 自定义协议）
  *
  * 硬件约束（用户确认）：
 *   - 8 路 RS485 对应 UART4/5/7/8、USART1/2/3/6，全部 115200 8N1。
 *   - 是否走 DMA 由具体 UART 决定：UART7/8 无 DMA、只能中断接收；其余 6 路有 DMA。
 *   - 逻辑通道 CH1..CH8 与 UART 的对应关系以板级硬件接线为准，见 sensor_hub.c 的 g_ports[]。
 *   - 每路均有 *_RTS_Pin 输出引脚，作为 RS485 收发方向控制（DE/RE）。
  *   - 传感器主动定时上传；集中器仅在需要时主动查询（功能码 04）。
  *
  * 协议约定（用户确认）：
  *   - 所有传感器从机地址统一为 0x01（每路独立总线）。
  *   - 类 Modbus 帧：地址(1) 功能码(1) 数据… CRC16(2，低字节在前)。
  *   - 上报/查询使用功能码 04（读输入寄存器），数据从 0x0000 开始。
  *   - 写保持寄存器使用功能码 06（预留，当前传感器无工作模式寄存器）。
  ******************************************************************************
  */
#ifndef __SENSOR_HUB_H
#define __SENSOR_HUB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "usart.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"

/* ============================ 传感器数量 ============================ */
#define SENSOR_HUB_COUNT        8
#define SENSOR_HUB_ADDR         0x01    /* 所有传感器统一从机地址 */

/* ============================ 类 Modbus 功能码 ============================ */
#define SH_FUNC_READ            0x04    /* 读输入寄存器 */
#define SH_FUNC_WRITE           0x06    /* 写单个保持寄存器（预留） */

/* ============================ 寄存器布局（0x0000 起始） ============================
 * 上报帧固定从 0x0000 开始，寄存器依次为：
 *   [0]  0x0000  voltage_flag   电压突变标志：0-无，1-有
 *   [1]  0x0001  temp_flag      温度突变标志：0-无，1-有
 *   [2]  0x0002  温度 float32 高 16 位
 *   [3]  0x0003  温度 float32 低 16 位    （单位 ℃，如 0x41D60000 = 21.375 ℃）
 *   [4-5]       （无寄存器，保留空隙；磁传感器电压需另路采集，不来自 RS485 寄存器）
 * 说明：协议 sensor_data 中保留 voltage 字段，但本期实现从 RS485 帧不取电压值。
 */
#define SH_REG_VOLTAGE_FLAG     0x0000
#define SH_REG_TEMP_FLAG        0x0001
#define SH_REG_TEMPERATURE      0x0002  /* 2 个寄存器，float32 */

/* 上报/查询的寄存器总数（电压寄存器无 → 仅 4 个寄存器） */
#define SH_UPLOAD_REG_COUNT     4

/* 写模式寄存器（预留，当前传感器无工作模式寄存器，暂不使用） */
#define SH_REG_MODE             0x0000  /* TODO：待确定后填入实际地址 */

/* 是否真正下发模式写帧（0=仅本地记录；1=发送功能码 06） */
#define SH_MODE_WRITE_ENABLE    0

/* ============================ 调试口占用（临时） ============================
 * 将某一路 RS485 临时改作调试打印口（printf / LOG_* 全部重定向到它）。
 *   - 取值 1..8：占用对应 sensor_id 的那一路做调试口，该路不再采集传感器；
 *   - 取值 0：  不占用任何路，全部 8 路正常采集（恢复正常功能）。
 * 切换只需改这一个宏。
 */
#ifndef SH_DEBUG_UART_ID
#define SH_DEBUG_UART_ID        1       /* 默认占用第 1 路（CH1 = UART7，中断接收那一路，临时调试用） */
#endif

/* 是否通过调试口打印每路收到的原始帧及解析结果（1=打印，0=不打印）。
 * 8 路同时上报时输出较密集，调试完建议改回 0。 */
#ifndef SH_DEBUG_DUMP_FRAME
#define SH_DEBUG_DUMP_FRAME     1
#endif

/* ============================ RS485 回显测试（临时诊断） ============================
 * PC 回显（loopback）测试：固件把每路（非调试口 SH_DEBUG_UART_ID）收到的数据原样发回，
 * 用户在 PC 端用 USB-RS485 转接板连到各路、发任意数据，看是否原样回显，
 * 即可验证该路 RX + TX + 方向脚(DE/RE) 全部正常。
 *   1 = 开启回显测试（不解析、不上报 TCP）；0 = 关闭（正常采集）。
 *
 * 重要：RS485 半双工且 DE/RE 绑定在同一方向脚，同口"自发自收"不可行，
 *       故用外部 PC 做回环。测试某路时该路建议不要接传感器（或忽略其自发上报），免干扰。
 * 调试口那一路(SH_DEBUG_UART_ID)本身作控制台，不对其回显；要测它，临时改 SH_DEBUG_UART_ID。
 */
#ifndef SH_LOOPBACK_ECHO
#define SH_LOOPBACK_ECHO        0
#endif

/* ============================ 接收缓冲 ============================ */
/* 最大帧长：地址1 + 功能码1 + 字节数1 + 数据12 + CRC2 = 17，取 64 足够 */
#define SH_RX_BUF_SIZE          64

/* 单帧接收缓冲（DMA/IT 共用），一帧完整数据 */
typedef struct {
    uint8_t  sensor_id;                 /* 传感器 ID（1..8） */
    uint16_t len;                       /* 有效字节数 */
    uint8_t  data[SH_RX_BUF_SIZE];      /* 帧数据 */
} SensorFrame_t;

/* ============================ 对外接口 ============================ */

/* 初始化：构建端口映射表、创建帧队列、启动全部接收（在任务上下文中调用） */
void SensorHub_Init(void);

/* 传感器采集任务：阻塞等待帧队列 → 解析 → 调用 tcp_send_sensor_data() 上报 */
void SensorHub_Task(const void *argument);

/* 主动查询某传感器（功能码 04，读 0x0000 起全部寄存器），结果经 RX 路径解析上报 */
int8_t SensorHub_Query(uint8_t sensor_id);

/* 写单个保持寄存器（功能码 06），供 mode_set 等未来功能使用 */
int8_t SensorHub_WriteReg(uint8_t sensor_id, uint16_t reg, uint16_t value);

/* 查询指定路的硬件信息（huart / DE 端口 / DE 引脚），供调试口复用。
 * sensor_id 越界时 *huart 置 NULL。 */
void SensorHub_GetPortHw(uint8_t sensor_id,
                         UART_HandleTypeDef **huart,
                         GPIO_TypeDef **de_port,
                         uint16_t *de_pin);

#ifdef __cplusplus
}
#endif

#endif /* __SENSOR_HUB_H */
