/*
 * This file is part of the Serial Flash Universal Driver Library.
 *
 * Copyright (c) 2016-2018, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: Portable interface for each platform.
 * Created on: 2016-04-23
 */

#include <stdarg.h>
#include <main.h>
#include "shell_port.h"
#include "spi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "../inc/sfud.h"

static char log_buf[256];

void sfud_log_debug(const char *file, const long line, const char *format, ...);

static void spi_lock(const sfud_spi *spi) {
	/* 裸机中使用开关中断方法 */
//    __disable_irq();
	/* OS 中使用临界区或互斥量，推荐互斥量方式 */
	//taskENTER_CRITICAL(); /* 进入临界区 */
	//vTaskSuspendAll();
}

static void spi_unlock(const sfud_spi *spi) {
	/* 裸机中使用开关中断方法 */
//    __enable_irq();
	/* OS 中使用临界区或互斥量，推荐互斥量方式 */
	//taskEXIT_CRITICAL(); /* 退出临界区 */
	//xTaskResumeAll();
}

/**
 * @brief 定义延时等待函数的实现,在RTOS环境下可以使用RTOS的延时机制,这里简单使用软件延时实现
 */
static void sfud_delay(void) {
	vTaskDelay(1);
}

/**
 * SPI write data then read data
 */
static sfud_err spi_write_read(const sfud_spi *spi, const uint8_t *write_buf, size_t write_size, uint8_t *read_buf,
		size_t read_size) {

	sfud_err result = SFUD_SUCCESS;

	/**
	 * add your spi write and read code
	 */
	if (write_size) {
		SFUD_ASSERT(write_buf);
	}
	if (read_size) {
		SFUD_ASSERT(read_buf);
	}

	if (read_size != 0) {
		HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_RESET);
		if (HAL_OK != HAL_SPI_Transmit(&hspi1, (uint8_t*) write_buf, write_size, 1000))
			result = SFUD_ERR_TIMEOUT;

		if (HAL_OK != HAL_SPI_Receive(&hspi1, (uint8_t*) read_buf, read_size, 1000))
			result = SFUD_ERR_TIMEOUT;
		HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_SET);
	} else {
		HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_RESET);
		if (HAL_OK != HAL_SPI_Transmit(&hspi1, (uint8_t*) write_buf, write_size, 1000))
			result = SFUD_ERR_TIMEOUT;
		HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_SET);
	}

	return result;
}

#ifdef SFUD_USING_QSPI
/**
 * read flash data by QSPI
 */
static sfud_err qspi_read(const struct __sfud_spi *spi, uint32_t addr, sfud_qspi_read_cmd_format *qspi_read_cmd_format,
        uint8_t *read_buf, size_t read_size) {
    sfud_err result = SFUD_SUCCESS;

    /**
     * add your qspi read flash data code
     */

    return result;
}
#endif /* SFUD_USING_QSPI */

sfud_err sfud_spi_port_init(sfud_flash *flash) {
	sfud_err result = SFUD_SUCCESS;

	switch (flash->index) {
		case SFUD_W25Q64CV_DEVICE_INDEX: {
			/* 同步 Flash 移植所需的接口及数据 */
			flash->spi.wr = spi_write_read;
			flash->spi.lock = spi_lock;
			flash->spi.unlock = spi_unlock;
			/* about 100 microsecond delay */
			flash->retry.delay = sfud_delay;
			/* adout 60 seconds timeout */
			flash->retry.times = 60 * 10000;
			break;
		}
	}

	return result;

}

/**
 * This function is print debug info.
 *
 * @param file the file which has call this function
 * @param line the line number which has call this function
 * @param format output format
 * @param ... args
 */
void sfud_log_debug(const char *file, const long line, const char *format, ...) {
	va_list args;

	/* args point to the first variable parameter */
	va_start(args, format);
	logInfo("[SFUD](%s:%ld) ", file, line);
	/* must use vprintf to print */
	vsnprintf(log_buf, sizeof(log_buf), format, args);
	logInfo("%s\n", log_buf);
	va_end(args);
}

/**
 * This function is print routine info.
 *
 * @param format output format
 * @param ... args
 */
void sfud_log_info(const char *format, ...) {
	va_list args;

	/* args point to the first variable parameter */
	va_start(args, format);
	logInfo("[SFUD]");
	/* must use vprintf to print */
	vsnprintf(log_buf, sizeof(log_buf), format, args);
	logInfo("%s\n", log_buf);
	va_end(args);
}
