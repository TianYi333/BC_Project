/**
 * @file system_service.h
 * @brief 系统服务头文件, 封装系统级方法
 */

#ifndef SYSTEM_SERVICE_H
#define SYSTEM_SERVICE_H

#include "main.h"

// STM32H743 唯一ID 地址定义
#define STM32H7_UID_ADDR0   ((uint32_t *)0x1FF1E800)
#define STM32H7_UID_ADDR1   ((uint32_t *)0x1FF1E804)
#define STM32H7_UID_ADDR2   ((uint32_t *)0x1FF1E808)

typedef struct
{
    uint32_t uid0;  // 第 0~3  字节
    uint32_t uid1;  // 第 4~7  字节
    uint32_t uid2;  // 第 8~11 字节
} STM32_UIDTypeDef;




/**
 * @fn void reboot_system(void)
 * @brief 重启系统
 */
void reboot_system(void);


/**
 * @fn void STM32_GetUID(STM32_UIDTypeDef *uid)
 * @brief 获取系统序列号
 */
void STM32_GetUID(STM32_UIDTypeDef *uid);


#endif /* SYSTEM_SERVICE_H */