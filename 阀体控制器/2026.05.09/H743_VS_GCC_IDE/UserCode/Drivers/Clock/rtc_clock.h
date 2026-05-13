/** @file rtc_clock.h
 * @brief RTC时钟驱动头文件
 * @author 梁伟
 * @date 2024-06-15
 */

#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include "rtc.h"
#include "time.h"

extern RTC_TimeTypeDef RtcTime; // RTC的时间
extern RTC_DateTypeDef RtcData; // RTC日期

/**
 * @fn void get_rtc_time()
 * @brief 获取RTC时间和日期
 *
 */
void refresh_rtc_time();
/**
 * @fn void Unix_To_Time(uint32_t)
 * @brief Unix时间戳解析并设置本地时间
 *
 * @param UnixNum Unix时间戳
 */
void Unix_To_Time(uint32_t UnixNum);
/**
 * @fn uint64_t Time_To_Unix()
 * @brief  本地时间生成Unix时间戳
 *
 * @return
 */
uint64_t Time_To_Unix();

#endif /* RTC_CLOCK_H */