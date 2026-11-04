/*
 * FAL port for the STM32F767 on-chip Flash.
 * Declaration of the flash device used to replace the previous
 * SPI/W25Q64 external-flash driver.
 */
#ifndef _FAL_FLASH_STM32F7_PORT_H_
#define _FAL_FLASH_STM32F7_PORT_H_

#include <fal.h>

extern struct fal_flash_dev stm32_onchip;

#endif /* _FAL_FLASH_STM32F7_PORT_H_ */
