

/** 
 * @file sht40.c
 *  @brief SHT40传感器驱动实现文件
 *  @author 梁伟
 *  @date 2024-06-15
 */

#include "sht40.h"

double_t Temperature = 0;
double_t Humidity = 0;

/** 
 * @brief 读取温湿度数据
 */
void SHT40_Read_Temperature_Humidity(double_t *Temperature,
                                     double_t *Humidity) {
  uint32_t Temperature_Byte;
  uint32_t Humidity_Byte;

  uint8_t I2C_Transmit_Data[1];
  I2C_Transmit_Data[0] = SHT40_CMD_MEASURE_HIGHREP;
  uint8_t I2C_Receive_Data[6];
  HAL_I2C_Master_Transmit(&hi2c1, SHT40_I2C_WRITE_ADDRESS, I2C_Transmit_Data, 1,
                          HAL_MAX_DELAY);
  HAL_Delay(10);
  HAL_I2C_Master_Receive(&hi2c1, SHT40_I2C_READ_ADDRESS, I2C_Receive_Data, 6,
                         HAL_MAX_DELAY);

  Temperature_Byte = I2C_Receive_Data[0] << 8 | I2C_Receive_Data[1];
  Humidity_Byte = I2C_Receive_Data[3] << 8 | I2C_Receive_Data[4];
  *Temperature = -45 + 175 * Temperature_Byte / 65535.0;
  *Humidity = -6 + 125 * Humidity_Byte / 65535.0;
}
/**
 * @brief 读取SHT40传感器的序列号
 * @return 序列号
 */
uint32_t SHT40_Read_Serial_Number() {
  uint32_t Serial_Number;
  uint8_t I2C_Transmit_Data[1];
  I2C_Transmit_Data[0] = SHT40_CMD_READ_SERIALNBR;
  uint8_t I2C_Receive_Data[6];
  HAL_I2C_Master_Transmit(&hi2c1, SHT40_I2C_WRITE_ADDRESS, I2C_Transmit_Data, 1,
                          HAL_MAX_DELAY);
  HAL_I2C_Master_Receive(&hi2c1, SHT40_I2C_READ_ADDRESS, I2C_Receive_Data, 6,
                         HAL_MAX_DELAY);
  Serial_Number = (I2C_Receive_Data[0] << 24) | (I2C_Receive_Data[1] << 16) |
                  (I2C_Receive_Data[3] << 8) | (I2C_Receive_Data[4] << 0);
  return Serial_Number;
}
/**
 * @brief 启用SHT40加热器
 */
void SHT40_Heater_200mW_1s() {
  uint8_t I2C_Transmit_Data[1];
  I2C_Transmit_Data[0] = SHT40_CMD_HEATER_ENABLE;
  HAL_I2C_Master_Transmit(&hi2c1, SHT40_I2C_WRITE_ADDRESS, I2C_Transmit_Data, 1,
                          HAL_MAX_DELAY);
}