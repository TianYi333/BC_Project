/** @file rtc_clock.c
 * @brief RTC时钟驱动头文件
 * @author 梁伟
 * @date 2024-06-15
 */

#include "rtc_clock.h"
#include "stdio.h"

RTC_TimeTypeDef RtcTime = {0}; // RTC的时间
RTC_DateTypeDef RtcData = {0}; // RTC日期
uint16_t RtcSubSec = 0;        // 新增：RTC亚秒值
/**
 * @fn void get_rtc_time()
 * @brief 获取RTC时间和日期
 *
 */
void refresh_rtc_time() {

  HAL_RTC_GetTime(&hrtc, &RtcTime, RTC_FORMAT_BIN); // 获取时间
  HAL_RTC_GetDate(&hrtc, &RtcData, RTC_FORMAT_BIN); // 获取日期
  RtcSubSec = (uint16_t)(RTC->SSR & RTC_SSR_SS);// 获取亚秒值
}

/**
 * @brief 毫秒Unix时间戳 转 RTC 时间日期（UTC+8 北京时间）
 * @param msUnix 毫秒级Unix时间戳(1970基准)
 */
void UnixMs_To_RTC(uint64_t msUnix)
{
  uint32_t unixSec = (uint32_t)(msUnix / 1000);
  uint16_t ms = (uint16_t)(msUnix % 1000);

  struct tm *timeStruct;
  RTC_DateTypeDef sDate = {0};
  RTC_TimeTypeDef sTime = {0};

  time_t count = unixSec;
  timeStruct = localtime(&count);
  if(timeStruct == NULL)
  {
      return;
  }

  // 日期转换
  sDate.Year   = timeStruct->tm_year - 100;
  sDate.Month  = timeStruct->tm_mon + 1;
  sDate.Date   = timeStruct->tm_mday;

  // 北京时间UTC+8 → 直接赋值时分秒（不再额外+8，避免重复偏移）
  sTime.Hours   = timeStruct->tm_hour;
  sTime.Minutes = timeStruct->tm_min;
  sTime.Seconds = timeStruct->tm_sec;

  // 设置时分秒、日期
  HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
  HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  // 1. 计算亚秒值
  uint16_t subSec = (uint16_t)((255ULL * (1000ULL - ms)) / 1000ULL);
  
  // 2. 进入RTC写保护模式
  HAL_RTCEx_EnableBypassShadow(&hrtc);
  __HAL_RTC_WRITEPROTECTION_DISABLE(&hrtc);
  
  // 3. 设置亚秒寄存器
  hrtc.Instance->SSR = subSec;
  
  // 4. 退出写保护
  __HAL_RTC_WRITEPROTECTION_ENABLE(&hrtc);
}

/**
 * @brief 获取当前 Unix 秒级时间戳（无毫秒，用于协议ts字段）
 */
uint64_t Get_Unix_Second(void)
{
    // 读取 RTC 得到毫秒时间戳，再取整为秒
    uint64_t ms_ts = RTC_To_UnixMs();
    return ms_ts / 1000ULL;
}

/**
 * @brief RTC 本地时间(UTC+8) 转 毫秒级Unix时间戳
 * @return 64位毫秒Unix时间戳
 */
uint64_t RTC_To_UnixMs(void)
{
  struct tm timeStruct;
  refresh_rtc_time();

  // 填充tm结构体：北京时间转UTC
  timeStruct.tm_year = RtcData.Year + 100;
  timeStruct.tm_mon  = RtcData.Month - 1;
  timeStruct.tm_mday = RtcData.Date;
  timeStruct.tm_hour = RtcTime.Hours;
  timeStruct.tm_min  = RtcTime.Minutes;
  timeStruct.tm_sec  = RtcTime.Seconds;

  // 得到秒级Unix时间戳
  time_t secStamp = mktime(&timeStruct);
  if(secStamp == (time_t)-1)
  {
      return 0;
  }

  // 亚秒转毫秒（适配 SynchPrediv = 255 常用配置）
  uint16_t ms = (uint16_t)((255ULL - RtcSubSec) * 1000ULL / 255ULL);

  // 拼接 秒*1000 + 毫秒
  return (uint64_t)secStamp * 1000ULL + ms;
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
