/**
 * @file system_service.c
 * @brief 系统服务源文件, 封装系统级方法
 */



#include "system_service.h"
#include "stdio.h"


/**
 * @fn void reboot_system(void)
 * @brief 重启系统
 */
void reboot_system(void)
{
    __set_FAULTMASK(1); // 关闭所有中断
    NVIC_SystemReset(); // 请求系统复位
}

/**
 * @fn void STM32_GetUID(STM32_UIDTypeDef *uid)
 * @brief 获取系统序列号
 */
void STM32_GetUID(STM32_UIDTypeDef *uid)
{
    uid->uid0 = *STM32H7_UID_ADDR0;
    uid->uid1 = *STM32H7_UID_ADDR1;
    uid->uid2 = *STM32H7_UID_ADDR2;
}
