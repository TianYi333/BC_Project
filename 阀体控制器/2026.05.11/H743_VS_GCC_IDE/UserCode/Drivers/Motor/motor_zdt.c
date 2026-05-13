/*
 * motor_zdt.c
 *
 *  Created on: Sep 25, 2025
 *      Author: 28038
 */

#include "motor_zdt.h"

/**
 * @fn int32_t zdt_get_motor_loc_abs(uint16_t, uint8_t, QueueHandle_t, uint8_t)
 * @brief 获取电机的相对位置
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信地址
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @return 电机的运行结果
 */
uint32_t zdt_get_motor_loc_abs(uint16_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate)
{

	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	uint32_t deg = 0;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.len = 2;
	MOTOR_MsgStructure.buffer[0] = 0x36;
	MOTOR_MsgStructure.buffer[1] = 0x6b;
	MOTOR_MsgStructure.rtr = DATA_FRAME;
	if (Select_Can_Send(interface, &MOTOR_MsgStructure) > 0)
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(500)))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0x36))
			{
				memcpy((void *)(&deg), (const void *)(MOTOR_MsgStructure.buffer + 2), 4);
				deg = BigtoLittle32(deg);
				deg = (deg / 10) / rate;
				return deg;
			}
			else
			{
				return -1;
			}
		}
		else
		{
			return -1;
		}
	}
	else
	{
		return -1;
	}
}
/**
 * @fn int8_t zdt_run_motor_loc_abs(uint8_t, uint8_t, QueueHandle_t, uint8_t, int16_t)
 * @brief 电机运行到指定位置（绝对位置）
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @param loc 电机运动的目标位置
 * @return 电机运行结果
 */
int8_t zdt_run_motor_loc_abs(uint8_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate, int16_t loc)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = addr * ex_addr;
	MOTOR_MsgStructure.len = 8;
	MOTOR_MsgStructure.rtr = FDCAN_DATA_FRAME;
	MOTOR_MsgStructure.buffer[0] = 0xFB;
	MOTOR_MsgStructure.buffer[1] = 0x01;
	MOTOR_MsgStructure.buffer[2] = 0x08;
	MOTOR_MsgStructure.buffer[3] = 0xFF;
	MOTOR_MsgStructure.buffer[4] = 0x00;
	MOTOR_MsgStructure.buffer[5] = 0x00;
	MOTOR_MsgStructure.buffer[6] = (loc * rate * 10) / 256;
	MOTOR_MsgStructure.buffer[7] = (loc * rate * 10) % 256;
	Select_Can_Send(interface, &MOTOR_MsgStructure);

	MOTOR_MsgStructure.id = ex_addr + 1;
	MOTOR_MsgStructure.len = 4;
	MOTOR_MsgStructure.buffer[0] = 0xFB;
	MOTOR_MsgStructure.buffer[1] = 0x01;
	MOTOR_MsgStructure.buffer[2] = 0x00;
	MOTOR_MsgStructure.buffer[3] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(2000)))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0xFB))
			{
				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:
					if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(2000)))
					{
						switch (MOTOR_MsgStructure.buffer[1])
						{
						case 0x9f:
							return 1;
						default:
							return 0;
						}
					}
					else
					{
						return 0;
					}
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn int8_t zdt_run_motor_loc_abs_t(uint8_t, uint8_t, QueueHandle_t, uint8_t, int16_t)
 * @brief 电机运行到指定位置（绝对位置）柔性控制
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @param loc 电机运动的目标位置
 * @return 电机运行结果
 */
int8_t zdt_run_motor_loc_abs_t(uint8_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate, int16_t loc)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = addr * ex_addr;
	MOTOR_MsgStructure.len = 8;
	MOTOR_MsgStructure.rtr = FDCAN_DATA_FRAME;
	MOTOR_MsgStructure.buffer[0] = 0xFD;
	MOTOR_MsgStructure.buffer[1] = 0x00;
	MOTOR_MsgStructure.buffer[2] = 0x02;
	MOTOR_MsgStructure.buffer[3] = 0xF0;
	MOTOR_MsgStructure.buffer[4] = 0x02;
	MOTOR_MsgStructure.buffer[5] = 0xF0;
	MOTOR_MsgStructure.buffer[6] = 0x08;
	MOTOR_MsgStructure.buffer[7] = 0xFF;
	Select_Can_Send(interface, &MOTOR_MsgStructure);

	MOTOR_MsgStructure.id = ex_addr + 1;
	MOTOR_MsgStructure.len = 8;
	MOTOR_MsgStructure.buffer[0] = 0xFD;
	MOTOR_MsgStructure.buffer[1] = 0x00;
	MOTOR_MsgStructure.buffer[2] = 0x00;
	MOTOR_MsgStructure.buffer[3] = (loc * rate * 10) / 256;
	MOTOR_MsgStructure.buffer[4] = (loc * rate * 10) % 256;
	MOTOR_MsgStructure.buffer[5] = 0x01;
	MOTOR_MsgStructure.buffer[6] = 0x00;
	MOTOR_MsgStructure.buffer[7] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(2000)))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0xFD))
			{
				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:
					if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(2000)))
					{
						switch (MOTOR_MsgStructure.buffer[1])
						{
						case 0x9f:
							return 1;
						default:
							return 0;
						}
					}
					else
					{
						return 0;
					}
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn int8_t zdt_run_motor_loc_rel(uint8_t, uint8_t, QueueHandle_t, uint8_t, int16_t, uint8_t)
 * @brief 电机按相对位置运行
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制命令的消息队列
 * @param rate 电机的减速比
 * @param deg 电机运行的相对角度
 * @param dir 电机的运行角度
 * @return 电机的运行结果
 */
int8_t zdt_run_motor_loc_rel(uint8_t addr, uint8_t interface, QueueHandle_t Queue, uint8_t rate, int16_t deg,
							 uint8_t dir)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.rtr = FDCAN_DATA_FRAME;
	MOTOR_MsgStructure.len = 8;
	MOTOR_MsgStructure.buffer[0] = 0xFB;
	MOTOR_MsgStructure.buffer[1] = dir;
	MOTOR_MsgStructure.buffer[2] = 0x08;
	MOTOR_MsgStructure.buffer[3] = 0xFF;
	MOTOR_MsgStructure.buffer[4] = 0x00;
	MOTOR_MsgStructure.buffer[5] = 0x00;
	MOTOR_MsgStructure.buffer[6] = (deg * rate * 10) / 256;
	MOTOR_MsgStructure.buffer[7] = (deg * rate * 10) % 256;
	Select_Can_Send(interface, &MOTOR_MsgStructure);

	MOTOR_MsgStructure.id = ex_addr + 1;
	MOTOR_MsgStructure.len = 4;
	MOTOR_MsgStructure.buffer[0] = 0xFB;
	MOTOR_MsgStructure.buffer[1] = 0x02;
	MOTOR_MsgStructure.buffer[2] = 0x00;
	MOTOR_MsgStructure.buffer[3] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(2000)))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0xFB))
			{

				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:
					if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(2000)))
					{
						switch (MOTOR_MsgStructure.buffer[1])
						{
						case 0x9f:
							return 1;
						default:
							return 0;
						}
					}
					else
					{
						return 0;
					}
					break;
				default:
					return 0;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn uint8_t zdt_set_motor_reset_zero(uint8_t, uint8_t, QueueHandle_t)
 * @brief 电机直接置零，设置当前位置为零点。
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_set_motor_reset_zero(uint8_t addr, uint8_t interface, QueueHandle_t Queue)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.len = 4;
	MOTOR_MsgStructure.buffer[0] = 0x93;
	MOTOR_MsgStructure.buffer[1] = 0x88;
	MOTOR_MsgStructure.buffer[2] = 0x01;
	MOTOR_MsgStructure.buffer[3] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, 1500))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0x93))
			{
				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:

					return 1;
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn uint8_t zdt_run_motor_zero(uint8_t, uint8_t, QueueHandle_t)
 * @brief 触发电机自动归零
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_run_motor_zero(uint8_t addr, uint8_t interface, QueueHandle_t Queue)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.len = 4;
	MOTOR_MsgStructure.buffer[0] = 0x9A;
	MOTOR_MsgStructure.buffer[1] = 0x00;
	MOTOR_MsgStructure.buffer[2] = 0x00;
	MOTOR_MsgStructure.buffer[3] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, 2000))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0x9A))
			{

				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:
					if (xQueueReceive(Queue, &MOTOR_MsgStructure, pdMS_TO_TICKS(5000)))
					{
						switch (MOTOR_MsgStructure.buffer[1])
						{
						case 0x9f:
							return 1;
						default:
							return 0;
						}
					}
					else
					{
						return 0;
					}
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn uint8_t zdt_set_motor_ma(uint8_t, uint8_t, QueueHandle_t)
 * @brief 设置电机闭环模式最大电流
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_set_motor_ma(uint8_t addr, uint8_t interface, QueueHandle_t Queue)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.len = 4;
	MOTOR_MsgStructure.buffer[0] = 0x45;
	MOTOR_MsgStructure.buffer[1] = 0x66;
	MOTOR_MsgStructure.buffer[2] = 0x01;
	MOTOR_MsgStructure.buffer[3] = 0x13;
	MOTOR_MsgStructure.buffer[4] = 0x88;
	MOTOR_MsgStructure.buffer[5] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, 500))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0x45))
			{
				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:

					return 1;
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn uint8_t zdt_release_motor_protect(uint8_t, uint8_t, QueueHandle_t)
 * @brief 释放电机保护
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_release_motor_protect(uint8_t addr, uint8_t interface, QueueHandle_t Queue)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.len = 3;
	MOTOR_MsgStructure.buffer[0] = 0x0e;
	MOTOR_MsgStructure.buffer[1] = 0x52;
	MOTOR_MsgStructure.buffer[2] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, 500))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0x0e))
			{
				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:

					return 1;
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn uint8_t zdt_restart_motor(uint8_t, uint8_t, QueueHandle_t)
 * @brief 重启电机
 *
 * @param addr 电机的通信地址
 * @param interface 电机的通信接口
 * @param Queue 电机控制的消息队列
 * @return 电机运行结果
 */
uint8_t zdt_restart_motor(uint8_t addr, uint8_t interface, QueueHandle_t Queue)
{
	uint16_t ex_addr = addr * 256;
	_CANMSG MOTOR_MsgStructure;
	MOTOR_MsgStructure.id = ex_addr;
	MOTOR_MsgStructure.len = 3;
	MOTOR_MsgStructure.buffer[0] = 0x08;
	MOTOR_MsgStructure.buffer[1] = 0x97;
	MOTOR_MsgStructure.buffer[2] = 0x6b;

	if (Select_Can_Send(interface, &MOTOR_MsgStructure))
	{
		if (xQueueReceive(Queue, &MOTOR_MsgStructure, 500))
		{
			if ((MOTOR_MsgStructure.id == ex_addr) && (MOTOR_MsgStructure.buffer[0] == 0x08))
			{
				switch (MOTOR_MsgStructure.buffer[1])
				{
				case 0x02:

					return 1;
					break;
				default:
					return 0;
					break;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	else
	{
		return 0;
	}
}

/**
 * @fn void Select_Can_Send(int8_t, _CANMSG*)
 * @brief 选择电机命令发送接口
 *
 * @param interface 电机接口编号
 * @param msg       消息内容
 */
uint8_t Select_Can_Send(int8_t interface, _CANMSG *msg)
{
	switch (interface)
	{
	case P_MOTOR_IF:
		return FDCAN1_Send_Msg(msg);
		break;
	case Q_MOTOR_IF:
		return FDCAN2_Send_Msg(msg);
		break;
	default:
		return 0;
		break;
	}
}
