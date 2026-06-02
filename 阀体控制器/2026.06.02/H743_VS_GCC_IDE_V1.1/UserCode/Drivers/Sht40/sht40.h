/*
 * @file: 文件名
 * @brief: 文件功能说明
 * @author: 你的名字（可填昵称/工号）
 * @date: 创建日期
 * @version: 版本号
 * @hardware: 适配硬件（如STM32F407ZGT6）
 * @note: 注意事项（如引脚分配、依赖外设）
 */
/** @file sht40.h
 *  @brief SHT40传感器驱动头文件
 *  @author 梁伟
 *  @date 2024-06-15
 */

#ifndef SHT40_H
#define SHT40_H

#include "i2c.h"
#include "main.h"


#define SHT40_I2C_WRITE_ADDRESS 0x44 << 1   // SHT40 I2C地址
#define SHT40_I2C_READ_ADDRESS  (0x44 << 1) | 0x01 // SHT40 I2C地址读取
#define SHT40_CMD_MEASURE_HIGHREP 0xFD // 高精度测量命令
#define SHT40_CMD_HEATER_ENABLE 0x39   // 启用加热器命令
#define SHT40_CMD_READ_SERIALNBR 0x89  // 读取序列号命令


/* 全局变量声明 */
extern double_t Temperature;
extern double_t Humidity;

/**
 * @brief 读取SHT40传感器的温湿度数据
 * @param Temperature 
 * @param Humidity 
 */
void SHT40_Read_Temperature_Humidity(double *Temperature, double *Humidity);
/**
 * @brief 读取SHT40传感器的序列号
 * @param  
 * @return 
 */
uint32_t SHT40_Read_Serial_Number(void);
/**
 * @brief 启用SHT40加热器
 * @param  
 */
void SHT40_Heater_200mW_1s(void);

#endif /* SHT40_H */