/*
 * @file           : boot_w25q.c
 * @brief          : W25Q64 极简 SPI 裸驱动（Bootloader 专用）
 * @note           : 仅用标准 SPI 命令（Read/PageProgram/SectorErase/BlockErase/
 *                   RDSR/WriteEnable），不依赖 Fal/SFUD/RTOS。SPI1 由 MX_SPI1_Init() 完成。
 */
#include "boot_w25q.h"
#include "spi.h"
#include "gpio.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define W25Q_CMD_READ          0x03
#define W25Q_CMD_WREN          0x06
#define W25Q_CMD_PP            0x02    /* Page Program */
#define W25Q_CMD_SECTOR_ERASE  0x20    /* 4KB */
#define W25Q_CMD_BLOCK_ERASE   0xD8    /* 64KB */
#define W25Q_CMD_RDSR          0x05
#define W25Q_PAGE_SIZE         256
#define W25Q_TMP_BUF           512

static void cs_low(void)
{
    HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_RESET);
}

static void cs_high(void)
{
    HAL_GPIO_WritePin(SPI1_NSS_GPIO_Port, SPI1_NSS_Pin, GPIO_PIN_SET);
}

static void spi_tx(const uint8_t *tx, size_t len)
{
    HAL_SPI_Transmit(&hspi1, (uint8_t *)tx, len, 1000);
}

static void spi_tx_rx(const uint8_t *tx, uint8_t *rx, size_t len)
{
    HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)tx, rx, len, 1000);
}

static uint8_t read_sr(void)
{
    uint8_t tx[2] = {W25Q_CMD_RDSR, 0xFF};
    uint8_t rx[2] = {0};

    cs_low();
    spi_tx_rx(tx, rx, 2);
    cs_high();
    return rx[1];
}

static void wait_busy(void)
{
    while (read_sr() & 0x01)
    {
        HAL_Delay(1);
    }
}

static void write_enable(void)
{
    uint8_t cmd = W25Q_CMD_WREN;

    cs_low();
    spi_tx(&cmd, 1);
    cs_high();
}

int w25q_read(uint32_t addr, uint8_t *buf, size_t len)
{
    uint8_t hdr[4] = {W25Q_CMD_READ, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    static uint8_t tmp[W25Q_TMP_BUF];
    size_t off = 0;

    while (len > 0)
    {
        size_t chunk = (len > W25Q_TMP_BUF) ? W25Q_TMP_BUF : len;

        cs_low();
        spi_tx(hdr, 4);
        memset(tmp, 0xFF, chunk);
        spi_tx_rx(tmp, tmp, chunk);
        memcpy(buf + off, tmp, chunk);
        cs_high();

        addr += (uint32_t)chunk;
        hdr[1] = (uint8_t)(addr >> 16);
        hdr[2] = (uint8_t)(addr >> 8);
        hdr[3] = (uint8_t)addr;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

int w25q_write(uint32_t addr, const uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (len > 0)
    {
        uint32_t page_off = addr & (W25Q_PAGE_SIZE - 1);
        size_t chunk = W25Q_PAGE_SIZE - page_off;

        if (chunk > len)
        {
            chunk = len;
        }

        write_enable();

        uint8_t hdr[4] = {W25Q_CMD_PP, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};

        cs_low();
        spi_tx(hdr, 4);
        spi_tx(buf + off, chunk);
        cs_high();
        wait_busy();

        addr += (uint32_t)chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

int w25q_erase_block64k(uint32_t addr)
{
    write_enable();
    uint8_t hdr[4] = {W25Q_CMD_BLOCK_ERASE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};

    cs_low();
    spi_tx(hdr, 4);
    cs_high();
    wait_busy();
    return 0;
}

int w25q_erase_sector4k(uint32_t addr)
{
    write_enable();
    uint8_t hdr[4] = {W25Q_CMD_SECTOR_ERASE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};

    cs_low();
    spi_tx(hdr, 4);
    cs_high();
    wait_busy();
    return 0;
}
