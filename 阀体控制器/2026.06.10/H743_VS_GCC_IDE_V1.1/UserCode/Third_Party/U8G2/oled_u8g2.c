#include "oled_u8g2.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>


uint8_t u8x8_byte_4wire_hw_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                               void *arg_ptr) {
  switch (msg) {
  case U8X8_MSG_BYTE_SEND: /*通过SPI发送arg_int个字节数据*/
    HAL_SPI_Transmit(&hspi2, (uint8_t *)arg_ptr, arg_int, 200);

    break;
  case U8X8_MSG_BYTE_INIT: /*初始化函数，这边我已经初始化SPI了就不填了*/
    break;
  case U8X8_MSG_BYTE_SET_DC: /*设置DC引脚，DC引脚控制发送的是数据还是命令*/
    HAL_GPIO_WritePin(OLED_DC_GPIO_Port, OLED_DC_Pin, arg_int);
    break;
  case U8X8_MSG_BYTE_START_TRANSFER: /*开始传输前会进行的操作，如果使用软件片选可以在这里进行控制*/
    u8x8_gpio_SetCS(u8x8, u8x8->display_info->chip_enable_level);
    u8x8->gpio_and_delay_cb(u8x8, U8X8_MSG_DELAY_NANO,
                            u8x8->display_info->post_chip_enable_wait_ns, NULL);
    break;
  case U8X8_MSG_BYTE_END_TRANSFER: /*传输后进行的操作，如果使用软件片选可以在这里进行控制*/
    u8x8->gpio_and_delay_cb(u8x8, U8X8_MSG_DELAY_NANO,
                            u8x8->display_info->pre_chip_disable_wait_ns, NULL);
    u8x8_gpio_SetCS(u8x8, u8x8->display_info->chip_disable_level);
    break;
  default:
    return 0;
  }
  return 1;
}

uint8_t u8x8_stm32_gpio_and_delay(U8X8_UNUSED u8x8_t *u8x8,
                                  U8X8_UNUSED uint8_t msg,
                                  U8X8_UNUSED uint8_t arg_int,
                                  U8X8_UNUSED void *arg_ptr) {
  switch (msg) {
  case U8X8_MSG_GPIO_AND_DELAY_INIT: /*dela和GPIO的初始化，我已经初始化过了*/
    break;
  case U8X8_MSG_DELAY_MILLI: /*延时ms函数*/
    vTaskDelay(arg_int);
    break;
  case U8X8_MSG_GPIO_CS: /*片选信号控制，但是似乎没有啥用*/
    HAL_GPIO_WritePin(OLED_SC_GPIO_Port, OLED_SC_Pin, arg_int);
    break;
  case U8X8_MSG_GPIO_DC: /*设置DC引脚，DC引脚控制发送的是数据还是命令*/
    HAL_GPIO_WritePin(OLED_DC_GPIO_Port, OLED_DC_Pin, arg_int);
    break;
  case U8X8_MSG_GPIO_RESET: /*GPIO复位*/
    HAL_GPIO_WritePin(OLED_RES_GPIO_Port, OLED_RES_Pin, arg_int);
    break;
  }
  return 1;
}
void u8g2Init(u8g2_t *u8g2) {

  // HAL_GPIO_WritePin(OLED_CS_GPIO_Port, OLED_CS_Pin, GPIO_PIN_RESET);
  // HAL_GPIO_WritePin(OLED_DC_GPIO_Port, OLED_DC_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(OLED_RES_GPIO_Port, OLED_RES_Pin, GPIO_PIN_RESET);
  vTaskDelay(1000);
  HAL_GPIO_WritePin(OLED_RES_GPIO_Port, OLED_RES_Pin, GPIO_PIN_SET);
  vTaskDelay(200);
  //	xSemaphoreBinary_u8g2 = xSemaphoreCreateBinary();

  u8g2_Setup_ssd1363_256x128_f(u8g2, U8G2_R0, u8x8_byte_4wire_hw_spi,
                               u8x8_stm32_gpio_and_delay);
  u8g2_InitDisplay(u8g2);
  u8g2_SetPowerSave(u8g2, 0);
  u8g2_ClearBuffer(u8g2);
}

void draw(u8g2_t *u8g2) {
  u8g2_ClearBuffer(u8g2);

  u8g2_SetFontMode(u8g2, 1);
  u8g2_SetFontDirection(u8g2, 0);
  u8g2_SetFont(u8g2, u8g2_font_inb24_mf);
  u8g2_DrawStr(u8g2, 0, 20, "U");

  u8g2_SetFontDirection(u8g2, 1);
  u8g2_SetFont(u8g2, u8g2_font_inb30_mn);
  u8g2_DrawStr(u8g2, 21, 8, "8");

  u8g2_SetFontDirection(u8g2, 0);
  u8g2_SetFont(u8g2, u8g2_font_inb24_mf);
  u8g2_DrawStr(u8g2, 51, 30, "g");
  u8g2_DrawStr(u8g2, 67, 30, "\xb2");

  u8g2_DrawHLine(u8g2, 2, 35, 47);
  u8g2_DrawHLine(u8g2, 3, 36, 47);
  u8g2_DrawVLine(u8g2, 45, 32, 12);
  u8g2_DrawVLine(u8g2, 46, 33, 12);

  u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
  u8g2_DrawStr(u8g2, 1, 54, "github.com/olikraus/u8g2");

  u8g2_SendBuffer(u8g2);
  vTaskDelay(1000);
}






/**
 * @brief  U8g2 格式化输出函数（类似 printf）
 * @param  u8g2: U8g2 设备句柄
 * @param  x: 绘制起始 x 坐标
 * @param  y: 绘制起始 y 坐标（U8g2 中是字符基线的 y 坐标，不是顶部）
 * @param  format: 格式化字符串（如 "数字：%d，字符串：%s"）
 * @param  ...: 可变参数列表
 * @retval 格式化后的字符串长度（失败返回-1）
 */
int u8g2_printf(u8g2_t *u8g2, u8g2_uint_t x, u8g2_uint_t y, const char *format, ...)
{

    char buf[U8G2_PRINTF_BUF_SIZE] = {0};
    

    // 1. 合法性校验（避免空指针导致崩溃）
    if (u8g2 == NULL || format == NULL)
    {
        return -1;
    }

    // 2. 定义临时缓冲区，存储格式化后的字符串


    // 3. 处理可变参数，格式化写入缓冲区
    va_list ap;
    va_start(ap, format);  // 初始化可变参数列表
    // 使用 vsnprintf 保证缓冲区安全，避免溢出
    int ret = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);  // 释放可变参数列表

    
    // 4. 调用 U8g2 接口，在屏幕上绘制格式化后的字符串
    if (ret > 0)  // 格式化成功（ret 为格式化后的字符串长度）
    {
        u8g2_DrawStr(u8g2, x, y, buf);
    }

    return ret;
}


// 简易浮点转字符串（num：浮点数值，decimals：保留小数位数，buf：输出缓冲区，buf_len：缓冲区大小）
void float2str(float num, uint8_t decimals, char *buf, uint16_t buf_len) {
    if (buf == NULL || buf_len < 8) return; // 预留足够缓冲区

    // 处理负数
    uint8_t is_negative = 0;
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }

    // 分离整数部分和小数部分
    int32_t integer_part = (int32_t)num;
    float decimal_part = num - integer_part;

    // 转换整数部分（简单倒序转字符串，再反转）
    char int_buf[16] = {0};
    uint8_t int_idx = 0;
    if (integer_part == 0) {
        int_buf[int_idx++] = '0';
    } else {
        while (integer_part > 0 && int_idx < 15) {
            int_buf[int_idx++] = (integer_part % 10) + '0';
            integer_part /= 10;
        }
    }

    // 反转整数部分，写入输出缓冲区
    uint16_t buf_idx = 0;
    if (is_negative) {
        buf[buf_idx++] = '-';
    }
    for (int8_t i = int_idx - 1; i >= 0 && buf_idx < buf_len - 1; i--) {
        buf[buf_idx++] = int_buf[i];
    }

    // 转换小数部分
    if (decimals > 0 && buf_idx < buf_len - 1) {
        buf[buf_idx++] = '.';
        for (uint8_t i = 0; i < decimals && buf_idx < buf_len - 1; i++) {
            decimal_part *= 10;
            uint8_t digit = (uint8_t)decimal_part;
            buf[buf_idx++] = digit + '0';
            decimal_part -= digit;
        }
    }

    // 字符串结束符
    buf[buf_idx < buf_len ? buf_idx : buf_len - 1] = '\0';
}