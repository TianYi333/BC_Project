#ifndef __STM32_U8G2_H
#define __STM32_U8G2_H

/* Includes ------------------------------------------------------------------*/
#include "u8g2.h"
#include "u8x8.h"
#include "spi.h"
/* USER CODE BEGIN Includes */

// 定义临时缓冲区大小（可根据需求调整，如256、512）
#define U8G2_PRINTF_BUF_SIZE 128
/* USER CODE END Includes */

/* USER CODE BEGIN Private defines */
/* USER CODE END Private defines */
#define u8         unsigned char
#define MAX_LEN    256  //
#define OLED_ADDRESS  0x78 // oled
#define OLED_CMD   0x00  //
#define OLED_DATA  0x40  //

/* USER CODE BEGIN Prototypes */
uint8_t u8x8_byte_hw_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
		void *arg_ptr);
uint8_t u8x8_gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
		void *arg_ptr);
void u8g2Init(u8g2_t *u8g2);
void draw(u8g2_t *u8g2);
int u8g2_printf(u8g2_t *u8g2, u8g2_uint_t x, u8g2_uint_t y, const char *format, ...);
void float2str(float num, uint8_t decimals, char *buf, uint16_t buf_len);

#endif
