/** @file rtc_clock.c
 * @brief RTC时钟驱动头文件
 * @author 梁伟
 * @date 2024-06-15
 */

#include "rtc_clock.h"
#include "stdio.h"

RTC_TimeTypeDef RtcTime = {0}; // RTC的时间
RTC_DateTypeDef RtcData = {0}; // RTC日期

/**
 * @fn void get_rtc_time()
 * @brief 获取RTC时间和日期
 *
 */
void refresh_rtc_time() {

  HAL_RTC_GetTime(&hrtc, &RtcTime, RTC_FORMAT_BIN); // 获取时间
  HAL_RTC_GetDate(&hrtc, &RtcData, RTC_FORMAT_BIN); // 获取日期
}

/**
 * @fn void Unix_To_Time(uint32_t)
 * @brief Unix时间戳解析并设置本地时间
 *
 * @param UnixNum Unix时间戳
 */
void Unix_To_Time(uint32_t unixTime) {
  struct tm *timeStruct;
  RTC_DateTypeDef sDate = {0};
  RTC_TimeTypeDef sTime = {0};

  time_t count = unixTime;
  timeStruct = localtime(&count);

  sDate.Year = timeStruct->tm_year - 100; // Adjust year
  sDate.Month = timeStruct->tm_mon + 1;   // Adjust month
  sDate.Date = timeStruct->tm_mday;

  sTime.Hours = timeStruct->tm_hour + 8; // Adjust for timezone (e.g., UTC+8)
  sTime.Minutes = timeStruct->tm_min;
  sTime.Seconds = timeStruct->tm_sec;

  HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
  HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
}

/**
 * @fn uint64_t Time_To_Unix()
 * @brief  本地时间生成Unix时间戳
 *
 * @return
 */
uint64_t Time_To_Unix() {

  struct tm timeStruct;

  refresh_rtc_time();

  timeStruct.tm_year = RtcData.Year + 100; // Adjust year
  timeStruct.tm_mon = RtcData.Month - 1;   // Adjust month
  timeStruct.tm_mday = RtcData.Date;
  timeStruct.tm_hour = RtcTime.Hours - 8; // Adjust for timezone (e.g., UTC+8)
  timeStruct.tm_min = RtcTime.Minutes;
  timeStruct.tm_sec = RtcTime.Seconds;

  return mktime(&timeStruct); // Returns Unix timestamp
}
