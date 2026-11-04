/*
 * running_logic.c
 *
 *  Created on: Sep 26, 2025
 *      Author: 28038
 */

#include "running_logic.h"
#include "Msg_Queue.h"
#include "buzzer.h"
#include "log.h"
#include "oled_task.h"
#include "rtc_clock.h"
#include "stm32h7xx_hal.h"
#include "timers.h"
#include <stdio.h>


__attribute__((section("._DTC_FSRAM"))) _INJECTOR_ERR_INFO injector_err_list[injector_count] = {0};// 全局故障数组
/**
 * 六代阀电机位置表,六代阀步进角度为90度，修改为如下定位方式
 */
_MOTOR_LOC motor_loc_list[12];

/**
 * 六代阀P模块定位
 */
_MOTOR_LOC motor_loc_list_p[12];
/**
 * 主要系统状态
 */
_SYS_STATUS main_sys_status;
/**
 * 任务对象列表，测试运行默认写入最后一个，自动任务默认顺序写入
 */
_TASK_OBJECT task_object_list[5];

/**
 * 注油器任务的句柄
 */
TaskHandle_t injector_task_handler;

TimerHandle_t xTimeHandle[injector_count];

/**
 * 注油任务的启动参数
 */
const osThreadAttr_t injector_task_attributes = {
    .name = "injector_task",
    .stack_size = injector_task_stk_size,
    .priority = injector_task_prio};

/**
 * @fn void start_injector_task(void)
 * @brief 启动注油器任务
 *
 */
void start_injector_task(void) {
  injector_task_handler = osThreadNew(injector_task, NULL, &injector_task_attributes);
}

/**
 * @fn void injector_task()
 * @brief 注油器的任务函数
 *
 */
void injector_task() {
  /**
   * 系统初始化，进行电机复位
   */
  uint8_t task_id = 0;
  // 先延时3秒，等待电机上电自检完成
  vTaskDelay(pdMS_TO_TICKS(3000));

  if (zdt_run_motor_zero(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg) != 1) {
    main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
    logInfo("Q reset failure .");
    show_oled_msg(OLED_MSG_ERROR, "Q reset failure .");
  }
  if (zdt_run_motor_zero(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg) != 1) {
    main_sys_status.sys_warning = P_MODEL_RESET_FAILURE;
    logInfo("P reset failure .");
    show_oled_msg(OLED_MSG_ERROR, "P reset failure .");
  }
  vTaskDelay(3000);
  /**
   * 进入任务循环
   */
  while (1) {
    /**
     * 通过系统状态选择执行动作
     */
    switch (main_sys_status.running_mode) {
    case SYS_MODE_STARTING:
      /**
       * 如果系统状态为初始化，则进行电机预备位置初始化。
       */
      show_oled_msg(OLED_MSG_UPDATE, "motor init ...");
      main_sys_status.is_Busy = 1;
      start_running();
      vTaskDelay(2000);
      main_sys_status.is_Busy = 0;
      break;
      /**
       * 如果系统状态为准备好，则扫描任务列表，执行任务。
       */
    case SYS_MODE_READY:
      for (task_id = 0;
           task_id < (sizeof(task_object_list) / sizeof(task_object_list[0]));
           task_id++) {
        if (task_object_list[task_id].task_status == 1) {
          /**
           * 执行任务列表里面的任务，并显示提示信息
           * 任务开始前，状态修改为忙碌，注油点任务状态修改为正在执行，任务完成后根据返回结果修改任务状态，并将系统状态修改为非忙碌。
           */
          main_sys_status.is_Busy = 1;
          // ==========记录当前执行的出油口ID==========
          g_current_running_inj_id = task_object_list[task_id].inject_id;
          // ======================================================
          main_sys_status.injector[task_object_list[task_id].inject_id - 1]
              .task_status = TASK_STATUS_RUNNING;
          show_oled_msg(OLED_MSG_UPDATE, "run task %d", task_id);
          uint32_t action_time =
              inject_running(task_object_list[task_id].val,
                             task_object_list[task_id].inject_id);
          if (action_time > 0) {
            buzz_contral(BUZZ_SHORT);
            // 任务执行成功，修改注油点任务状态为成功，并记录执行性能指标（注油时间）
            main_sys_status.injector[task_object_list[task_id].inject_id - 1]
                .task_status = TASK_STATUS_SUCCESS;
            main_sys_status.injector[task_object_list[task_id].inject_id - 1]
                .execution_performance = action_time;
            task_object_list[task_id].task_status = 0;
          } else {
            buzz_contral(BUZZ_LONG);
            // 任务执行失败，修改注油点任务状态为失败，并记录执行性能指标（注油时间）
            main_sys_status.injector[task_object_list[task_id].inject_id - 1]
                .task_status = TASK_STATUS_FAILURE;
            main_sys_status.injector[task_object_list[task_id].inject_id - 1]
                .execution_performance = action_time;
            task_object_list[task_id].task_status = 0;          
          }
          g_current_running_inj_id = 0;// 任务执行完毕，清空当前运行出油口标记
          main_sys_status.is_Busy = 0;
        }
      }

      break;
      /**
       * 如果系统状态为监控，则扫描任务列表，执行任务。
       */
    case SYS_MODE_MONITOR:
      for (task_id = 0;
           task_id < (sizeof(task_object_list) / sizeof(task_object_list[0]));
           task_id++) {
        if (task_object_list[task_id].task_status == 1) {
          /**
           * 执行任务列表里面的任务，并显示提示信息
           */
          buzz_contral(BUZZ_SHORT);
          main_sys_status.is_Busy = 1;
          g_current_running_inj_id = task_object_list[task_id].inject_id;
          show_oled_msg(OLED_MSG_UPDATE, "run task %d", task_id);
          inject_running(task_object_list[task_id].val,
                         task_object_list[task_id].inject_id);
          task_object_list[task_id].task_status = 0;
          g_current_running_inj_id = 0;// 任务执行完毕，清空当前运行出油口标记
          main_sys_status.is_Busy = 0;
        }
      }
      break;
    case SYS_MODE_SETTING:
      break;
    case SYS_MODE_AUTO:
      break;
    }
    
    // //在空闲时主动刷新传感器状态
    // if (main_sys_status.is_Busy == 0) 
    // {
    //   uint8_t current_sen = Read_MET_Sensor();
    //   if (current_sen == 1 || current_sen == 2) {
    //       main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
    //       main_sys_status.q_model.sen = current_sen;
    //   }
    // }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
/**
 * @fn void injector_timer()
 * @brief 创建注油器定时器
 *
 */
void injector_timer_init() {
  uint8_t tag;
  for (tag = 0; tag < injector_count; tag++) {
    xTimeHandle[tag] =
        xTimerCreate("injector", 30 * 1000, pdTRUE,
                     &main_sys_status.injector[tag].injector_id,
                     (TimerCallbackFunction_t)injector_task_maker);
        // 创建成功就启动定时器
        if(xTimeHandle[tag] != NULL)
        {
            // portMAX_DELAY：阻塞等待定时器命令队列空闲
            xTimerStart(xTimeHandle[tag], portMAX_DELAY);
        }
  }
}
/**
 * @fn void injector_task_maker(TimerHandle_t)
 * @brief 定时任务请求函数（注油器定时器的回调）
 *
 * @param xTimer
 */
void injector_task_maker(TimerHandle_t xTimer) {
	uint16_t *p_id = 0;
	p_id = (uint16_t *)pvTimerGetTimerID(xTimer);
    if(p_id != NULL)//判断指针地址非空
    {
        uint16_t no = *p_id;//获取指针指向的值，即注油器ID
        if(no >= 1 && no <= injector_count)
        {
            main_sys_status.injector[no - 1].injectRequest = 99;
            TaskInfo_t tmp;
            memset(&tmp,0,sizeof(tmp));
            tmp.inj_id = no;

            if(xTimerReqQueue != NULL)
            {
                xQueueSend(xTimerReqQueue, &tmp, 0); //非阻塞入队
            }
        }
    }
}

/**
 * @fn int8_t start_running(void)
 * @brief 初始化P模块电机，进入预备状态
 *
 */
uint8_t start_running(void) {

  /**
   * 先进行P模块的自检,确保Q模块自检时油路通畅
   */
  if (p_motor_run_point(motor_loc_list_p[1]) != 1) {
    main_sys_status.sys_warning = P_MODEL_RESET_FAILURE;
    return 0;
  }
  /**
   * 进行Q模块复位，检查传感器位置和电机位置，判断是否需要进行阀芯复位
   */
  if (Read_MET_Sensor() == 0) {
    /**
     * 如果Q模块传感器不在限位，则转动到0点等待传感器到限位
     */
    if (q_motor_run_point(motor_loc_list[0]) == 1) {
      /**
       * 电机转到开放角度，等待阀芯传感器信号
       */
      // printf("waiting sen action...");
      show_oled_msg(OLED_MSG_UPDATE, "waiting sen ...");
      /**
       * 强制确认传感器位置
       */
      if (waiting_met_sen(motor_loc_list[0].pipe) > 0) {
        /**
         * 传感器位置正确，记录传感器位置
         */
        main_sys_status.q_model.sen = Read_MET_Sensor();
        /**
         * 电机运行到运行预备点
         */
        if (q_motor_run_point(motor_loc_list[1]) != 1) {
          /**
           * 电机运行到预备点失败，报错返回
           */
          main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
          main_sys_status.running_mode = SYS_MODE_STARTING;
          show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
          return 0;
        } else {
          /**
           * 电机运行到预备点成功，返回
           */
          show_oled_msg(OLED_MSG_UPDATE, "p ready to %d", motor_loc_list[2].id);

          // printf("motor ready to %d \n", motor_loc_list[2].id);
          main_sys_status.running_mode = SYS_MODE_READY;
          return 1;
        }
      } else {
        /**
         * 传感器确认失败，报错返回
         */
        main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
        main_sys_status.running_mode = SYS_MODE_STARTING;
        show_oled_msg(OLED_MSG_ERROR, "init sen error ...");
        return 0;
      }
    } else {
      main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
      main_sys_status.running_mode = SYS_MODE_STARTING;
      show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
      return 0;
    }
  } else {
    /**
     * 检查传感器位置和电机角度是否对应，当前电机位于初始位置，如果传感器位置与1点重叠，则需要运动到2点作为运行预位
     */
    if (Read_MET_Sensor() == motor_loc_list[0].pipe) {
      if (q_motor_run_point(motor_loc_list[1]) != 1) {
        main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
        main_sys_status.running_mode = SYS_MODE_STARTING;
        show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
        return 0;
      }
      // printf("motor ready to %d \n", motor_loc_list[2].id);
      show_oled_msg(OLED_MSG_UPDATE, "p ready to %d", motor_loc_list[2].id);

      // printf("motor ready to %d \n", motor_loc_list[2].id);
      main_sys_status.running_mode = SYS_MODE_READY;
      return 1;
    } else {
      if (q_motor_run_point(motor_loc_list[3]) != 1) {
        main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
        main_sys_status.running_mode = SYS_MODE_STARTING;
        show_oled_msg(OLED_MSG_ERROR, "init motor error ...");
        return 0;
      }
      // printf("motor ready to %d \n", motor_loc_list[3].id);
      show_oled_msg(OLED_MSG_UPDATE, "p ready to %d", motor_loc_list[3].id);

      // printf("motor ready to %d \n", motor_loc_list[3].id);
      main_sys_status.running_mode = SYS_MODE_READY;
      return 1;
    }
  }
}

/**
 * @brief  内部通用：填充并上报执行结果
 * @note   封装所有分支重复的上报逻辑，避免冗余代码
 */
static void inline_report_result(uint8_t injector_id, uint32_t fill_cur,
                                 uint32_t single_cost_ms, uint32_t total_cost_ms,
                                 uint16_t valve_temp_val, uint16_t valve_hum_val,
                                 uint8_t execute_result, uint8_t error_code,
                                 const char *error_msg)
{
    ExecuteResultInfo_t exec_info; // 局部变量，无并发风险
    memset(&exec_info, 0, sizeof(exec_info));

    // 任务ID绑定
    if(g_tcp_task_bind.bind_inj_id != 0 && g_tcp_task_bind.bind_inj_id == injector_id)
    {
        strncpy(exec_info.task_id, g_tcp_task_bind.task_id, sizeof(exec_info.task_id)-1);
    }
    else
    {
        strncpy(exec_info.task_id, "0", sizeof(exec_info.task_id)-1);
    }
    exec_info.task_id[sizeof(exec_info.task_id)-1] = '\0';

    exec_info.inj_id = injector_id;
    exec_info.actual_volume = fill_cur;
    exec_info.valve_act_tm = single_cost_ms;
    exec_info.valve_total_tm = total_cost_ms;
    exec_info.valve_temp = valve_temp_val;
    exec_info.valve_hum = valve_hum_val;
    exec_info.execute_result = execute_result;
    exec_info.error_code = error_code;

    strncpy(exec_info.error_msg, error_msg, sizeof(exec_info.error_msg)-1);
    exec_info.error_msg[sizeof(exec_info.error_msg)-1] = '\0';

    tcp_send_execute_result(&exec_info, 1U);
}

/**
 * @fn uint8_t inject_running(uint8_t, uint8_t)
 * @brief 注油运行
 *
 * @param inject_val   注入量
 * @param injector_id  出口ID
 * @return
 */
uint32_t inject_running(uint16_t inject_val, uint8_t injector_id)
{
  // ========== 1. 入口参数合法性校验（防止数组越界） ==========
  if(injector_id == 0 || injector_id > ARRAY_SIZE(main_sys_status.injector))
  {
      return 0;
  }
  uint32_t action_time = 0;     //单次平均耗时
  uint32_t single_start_tick;   // 单次单mL起始时间戳
  uint32_t single_cost_ms = 0;  // 单次实际耗时
  uint32_t total_cost_ms = 0;   // 累计总耗时
  uint16_t tag = 0;
  uint32_t fill_cur = 0;        //注油量计数

//--------------------------------温湿度：直接读取定时任务全局缓存，无I2C阻塞------------------------------------------------------------------------------------------------
  uint16_t valve_temp_val = g_sht40_temp_cache;
  uint16_t valve_hum_val  = g_sht40_hum_cache;
//-------------------------------------------------------------------------------------------------------------------------------
  main_sys_status.injector[injector_id - 1].executionVol = inject_val;
  main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：任务开始先清零进度

  show_oled_msg(OLED_MSG_UPDATE, "task %u-%lu", injector_id, (unsigned long)inject_val);
  /**
   * 判断Q模块位置是不是关闭点
   */
  if (q_motor_run_close() == 0) {
    show_oled_msg(OLED_MSG_ERROR, "motor error ...");
    main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
//-------------------------------失败上报------------------------------------------------------------------------------------------------
    inline_report_result(injector_id, fill_cur, single_cost_ms, total_cost_ms,
                          valve_temp_val, valve_hum_val, 3, 1,
                          "Q_MODEL_RUN_CLOSE_FAILURE !"); 
    // 清空当前运行出油口标记
    g_current_running_inj_id = 0;
    //注油结束，清空全局任务ID
    memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
    LOG("Oil filling FAIL, inj_id:%u, err_code:%u, msg:%s\r\n", injector_id, exec_info.error_code, exec_info.error_msg);
    // 记录当前出油口故障
    record_injector_err(injector_id, exec_info.error_code, "Q_MODEL_RESET_FAILURE");
    main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：失败退出清零
    vTaskDelay(200);
//-------------------------------------------------------------------------------------------------------------------------------
    return 0;
  }
  /**
   * 先选择出油口，再进行供油，完成后P模块复位
   */
  if (p_motor_run_point(motor_loc_list_p[injector_id - 1]) == 0) {
    main_sys_status.sys_warning = P_MODEL_RESET_FAILURE;
    show_oled_msg(OLED_MSG_ERROR, "P motor error ...");
    // P模块运行失败的处理
//-------------------------------失败上报------------------------------------------------------------------------------------------------
    inline_report_result(injector_id, fill_cur, single_cost_ms, total_cost_ms,
                          valve_temp_val, valve_hum_val, 3, 2,
                          "P_MODEL_RUN_POINT_FAILURE !");
    // 清空当前运行出油口标记
    g_current_running_inj_id = 0;
    //注油结束，清空全局任务ID
    memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
    LOG("Oil filling FAIL, inj_id:%u, err_code:%u, msg:%s\r\n", injector_id, exec_info.error_code, exec_info.error_msg);
    // 记录当前出油口故障
    record_injector_err(injector_id, exec_info.error_code, "P_MODEL_RESET_FAILURE");
    main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：失败退出清零
    vTaskDelay(200);
    //-------------------------------------------------------------------------------------------------------------------------------

    return 0;
  }

  for (tag = inject_val; tag > 0; tag--) {
    // 获取起始时间
    uint32_t s_time = HAL_GetTick();
    single_start_tick = HAL_GetTick();
    /**
     * Q电机从预备点运行到下一个开放点
     */
    if (q_motor_run_open() != 1) {
      /**
       * 电机运行失败，任务失败返回。
       */
      show_oled_msg(OLED_MSG_ERROR, "motor error ...");
      main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;

//-------------------------------失败上报------------------------------------------------------------------------------------------------
      inline_report_result(injector_id, fill_cur, single_cost_ms, total_cost_ms,
                            valve_temp_val, valve_hum_val, 3, 3,
                            "Q_MODEL_RUN_OPEN_FAILURE !");
      // 清空当前运行出油口标记
      g_current_running_inj_id = 0;
      //注油结束，清空全局任务ID
      memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
      LOG("Oil filling FAIL, inj_id:%u, err_code:%u, msg:%s\r\n", injector_id, exec_info.error_code, exec_info.error_msg);
      // 记录当前出油口故障
      record_injector_err(injector_id, exec_info.error_code, "Q_MODEL_RESET_FAILURE");
      main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：失败退出清零
//-------------------------------------------------------------------------------------------------------------------------------

      vTaskDelay(2000);
      return 0;
    }
    /**
     * 电机转到开放角度，等待阀芯传感器信号
     */
    printf("met sen go %d \n", main_sys_status.q_model.point.pipe);

    if (waiting_met_sen(main_sys_status.q_model.point.pipe) == 0) {
      /**
       * 没有收到金属传感器中断
       */
      printf("met sen error \n");
      show_oled_msg(OLED_MSG_ERROR, "met sen error");
      main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;

//-------------------------------失败上报------------------------------------------------------------------------------------------------
      inline_report_result(injector_id, fill_cur, single_cost_ms, total_cost_ms,
                            valve_temp_val, valve_hum_val, 3, 4,
                            "WATING_MET_SEN_FAILURE !");
      // 清空当前运行出油口标记
      g_current_running_inj_id = 0;
      //注油结束，清空全局任务ID
      memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
      LOG("Oil filling FAIL, inj_id:%u, err_code:%u, msg:%s\r\n", injector_id, exec_info.error_code, exec_info.error_msg);
      // 记录当前出油口故障
      record_injector_err(injector_id, exec_info.error_code, "Q_MODEL_RESET_FAILURE");
      main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：失败退出清零
      vTaskDelay(200);
      //-------------------------------------------------------------------------------------------------------------------------------
      return 0;
    }
    // 获取动作时间
    if (action_time == 0) {
      action_time = HAL_GetTick() - s_time;
    } else {
      action_time = (action_time + HAL_GetTick() - s_time) / 2;
    }

//-------------------------------单次上报------------------------------------------------------------------------------------------------
    fill_cur++;
    single_cost_ms = HAL_GetTick() - single_start_tick;
    total_cost_ms += single_cost_ms;
    //执行结果上报eth
    inline_report_result(injector_id, fill_cur, single_cost_ms, total_cost_ms,
                          valve_temp_val, valve_hum_val, 1, 0, "OK");
    LOG("Filling progress: current %lu mL / total %lu mL\r\n", (unsigned long)fill_cur, (unsigned long)inject_val);
    main_sys_status.injector[injector_id - 1].current_progress = fill_cur;  // 新增：实时更新进度    
//-------------------------------------------------------------------------------------------------------------------------------

    main_sys_status.injector[injector_id - 1].executionVol = tag - 1;
    show_oled_msg(OLED_MSG_UPDATE, "task %d-%d-%d", injector_id, tag - 1,
                  action_time);
    buzz_contral(BUZZ_SHORT);
    // vTaskDelay(100);
  }
  /**
   * 循环内部不在关闭位置停止，单点注油完成后回关闭位置
   */
  if (q_motor_run_close() == 0) {
    show_oled_msg(OLED_MSG_ERROR, "motor error ...");
    main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;

//-------------------------------失败上报------------------------------------------------------------------------------------------------
    inline_report_result(injector_id, fill_cur, single_cost_ms, total_cost_ms,
                          valve_temp_val, valve_hum_val, 3, 5,
                          "Q_MODEL_RETURN_CLOSE_FAILURE !");
    // 清空当前运行出油口标记
    g_current_running_inj_id = 0;
    //注油结束，清空全局任务ID
    memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
    LOG("Oil filling FAIL, inj_id:%u, err_code:%u, msg:%s\r\n", (unsigned int)injector_id, (unsigned int)exec_info.error_code, exec_info.error_msg);
    // 记录当前出油口故障
    record_injector_err(injector_id, exec_info.error_code, "Q_MODEL_RESET_FAILURE");
    main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：失败退出清零
    vTaskDelay(200);
    //-------------------------------------------------------------------------------------------------------------------------------
    
    return 0;
  }
  vTaskDelay(100);//短暂延时
//---------------------------------成功上报----------------------------------------------------------------------------------------------
  inline_report_result(injector_id, fill_cur, action_time, total_cost_ms,
                        valve_temp_val, valve_hum_val, 2, 0,
                        "Filling complete successfully");
  // 清空当前运行出油口标记
  g_current_running_inj_id = 0;
  //注油结束，清空全局任务ID
  memset(&g_tcp_task_bind, 0, sizeof(g_tcp_task_bind));
  LOG("Oil filling finished, inj_id:%u, actual total vol:%lu mL, total run time:%lu ms\r\n",
      injector_id, (unsigned long)fill_cur, (unsigned long)total_cost_ms);
  //执行成功，清除该通道历史错误码、错误描述
  clear_injector_err(injector_id);
  //main_sys_status.injector[injector_id - 1].current_progress = 0;  // 新增：成功完成清零
//-------------------------------------------------------------------------------------------------------------------------------

  return action_time;
}

//记录故障
void record_injector_err(uint8_t inj_id, uint8_t err_code, const char *err_msg)
{
    if (inj_id < 1 || inj_id > injector_count)
        return;
    uint8_t tag = inj_id - 1;
    _INJECTOR_ERR_INFO *p_err = &injector_err_list[tag];

    p_err->err_code = err_code;
    strncpy(p_err->err_msg, err_msg, sizeof(p_err->err_msg) - 1);
    p_err->err_msg[sizeof(p_err->err_msg) - 1] = '\0';
}

//清除单路故障
void clear_injector_err(uint8_t inj_id)
{
    if (inj_id < 1 || inj_id > injector_count)
        return;
    uint8_t tag = inj_id - 1;
    // 整块清零：err_code=0，err_msg全部置空
    memset(&injector_err_list[tag], 0, sizeof(_INJECTOR_ERR_INFO));
}

/**
 * @fn int8_t p_motor_run_point(_MOTOR_LOC)
 * @brief P阀电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t p_motor_run_point(_MOTOR_LOC point) {
  if (zdt_run_motor_loc_abs_t(P_MOTOR_ADDR, P_MOTOR_IF, msgQueue_ID_CAN1,
                              P_MOTOR_RATE, point.deg) == 1) {
    main_sys_status.p_model.point = point;
    main_sys_status.p_model.status = MOTOR_STATUS_READY;
    return 1;
  } else {
    show_oled_msg(OLED_MSG_ERROR, "P running failure .");
    main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
    main_sys_status.p_model.status = MOTOR_STATUS_PROTECTED;
    show_oled_msg(OLED_MSG_ERROR, "P motor release protect .");
    // 进行解除保护操作
    zdt_release_motor_protect(P_MOTOR_ADDR, P_MOTOR_IF, msgQueue_ID_CAN1);
    // 再次尝试运行到指定位置
    if (zdt_run_motor_loc_abs_t(P_MOTOR_ADDR, P_MOTOR_IF, msgQueue_ID_CAN1,
                                P_MOTOR_RATE, point.deg) == 1) {
      main_sys_status.p_model.point = point;
      main_sys_status.p_model.status = MOTOR_STATUS_READY;
      return 1;
    } else {
      show_oled_msg(OLED_MSG_ERROR, "P running failure .");
      main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
      main_sys_status.p_model.status = MOTOR_STATUS_OFFLINE;
      return 0;
    }
  }
}

/**
 * @fn int8_t q_motor_run_point(_MOTOR_LOC)
 * @brief Q电机运行到指定位置
 *
 * @param point 角度坐标
 * @return
 */
int8_t q_motor_run_point(_MOTOR_LOC point) {
  if (zdt_run_motor_loc_abs_t(Q_MOTOR_ADDR, Q_MOTOR_IF, msgQueue_ID_CAN2,
                              Q_MOTOR_RATE, point.deg) == 1) {
    printf("Q motor run to N.%d deg.%d sen.%d\n", point.id, point.deg,
           point.pipe);
    main_sys_status.q_model.point = point;
    main_sys_status.q_model.motor_status = MOTOR_STATUS_READY;
    return 1;
  } else {
    printf("Q motor err to N.%d deg.%d sen.%d\n", point.id, point.deg,
           point.pipe);
    main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
    main_sys_status.q_model.motor_status = MOTOR_STATUS_PROTECTED;
    show_oled_msg(OLED_MSG_ERROR, "Q motor release protect .");

    // 进行解除保护操作
    zdt_release_motor_protect(Q_MOTOR_ADDR, Q_MOTOR_IF, msgQueue_ID_CAN2);
    // 再次尝试运行到指定位置
    if (zdt_run_motor_loc_abs_t(Q_MOTOR_ADDR, Q_MOTOR_IF, msgQueue_ID_CAN2,
                                Q_MOTOR_RATE, point.deg) == 1) {
      main_sys_status.q_model.point = point;
      main_sys_status.q_model.motor_status = MOTOR_STATUS_READY;
      return 1;
    } else {
      show_oled_msg(OLED_MSG_ERROR, "Q running failure .");
      main_sys_status.sys_warning = MOTOR_RUNNING_FAILURE;
      main_sys_status.q_model.motor_status = MOTOR_STATUS_OFFLINE;
      return 0;
    }
  }
}
/**
 * @fn int8_t q_motor_run_close()
 * @brief Q模块运行到关闭点
 *
 * @return
 */
int8_t q_motor_run_close() {
  /**
   * 判断Q模块是不是在关闭位置
   */
  if (main_sys_status.q_model.point.pipe != 0) {
    /**
     * 判断是否到达反转点
     */
    switch (main_sys_status.q_model.point.id) {
    case 8:
      main_sys_status.q_model.dir = -1;
      break;
    case 2:
      main_sys_status.q_model.dir = 1;
      break;
    default:
      break;
    }
    /**
     * Q模块运行到下一点，即关闭点。
     */
    if (q_motor_run_point(motor_loc_list[main_sys_status.q_model.point.id +
                                         main_sys_status.q_model.dir]) != 1) {
      show_oled_msg(OLED_MSG_ERROR, "motor error ...");
      main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
      return 0;
    } else {
      return 1;
    }
  } else {
    return 1;
  }
}
/**
 * @fn int8_t q_motor_run_open()
 * @brief  Q模块运行到开放点
 *
 * @return
 */
int8_t q_motor_run_open() {
  /**
   * 判断Q模块是不是在关闭位置
   */
  if (main_sys_status.q_model.point.pipe != 0) {
    /**
     * 判断是否到达反转点
     */
    switch (main_sys_status.q_model.point.id) {
    case 8:
      main_sys_status.q_model.dir = -1;
      break;
    case 2:
      main_sys_status.q_model.dir = 1;
      break;
    default:
      break;
    }
    /**
     * Q模块运行到下一个开放点。
     */
    if (q_motor_run_point(motor_loc_list[main_sys_status.q_model.point.id +
                                         (main_sys_status.q_model.dir * 2)]) !=
        1) {
      show_oled_msg(OLED_MSG_ERROR, "motor error ...");
      main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
      return 0;
    } else {
      return 1;
    }
  } else {
    /**
     * Q模块运行到下一点。
     */
    if (q_motor_run_point(motor_loc_list[main_sys_status.q_model.point.id +
                                         main_sys_status.q_model.dir]) != 1) {
      show_oled_msg(OLED_MSG_ERROR, "motor error ...");
      main_sys_status.sys_warning = Q_MODEL_RESET_FAILURE;
      return 0;
    } else {
      return 1;
    }
  }
}

/**
 * @fn uint8_t Read_Sensor(void)
 * @brief 读取阀芯限位传感器，如果两个传感器都无效则输出0
 *
 * @return
 */
int8_t Read_MET_Sensor(void) {
  if (READ_MET_SEN_1 == GPIO_PIN_SET) {
    if (READ_MET_SEN_2 == GPIO_PIN_RESET) {
      /**
       * 传感器状态写入寄存器
       */
      main_sys_status.q_model.sen = 1;
      main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
      return 1;
    } else {
      /**
       * 
       如果两个传感器都有效，设备故障
       */
      main_sys_status.q_model.sen = 0;
      main_sys_status.q_model.sen_status = SENSOR_STATUS_D_OFFLINE;
      return 0;
    }
  } else {
    if (READ_MET_SEN_2 == GPIO_PIN_RESET) {
      /**
       * 如果两个传感器都无效，阀芯没有复位或者设备故障
       */
      main_sys_status.q_model.sen = 0;
      return 0;
    } else {
      /**
       * 传感器状态写入寄存器
       */
      main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
      main_sys_status.q_model.sen = 2;
      return 2;
    }
  }
}

/**
 * @fn uint8_t waiting_met_sen(uint8_t)
 * @brief 强制确认传感器位置（直接读取传感器，如果读取确认失败就等待传感器中断）
 *
 * @param sen_id 需要确认的传感器ID
 * @return
 */
uint8_t waiting_met_sen(uint8_t sen_id) {
  uint8_t action_sen_id;
  uint32_t tick_s = HAL_GetTick();
  //xQueueReset(QUEUE_MET_SEN);
  printf("waiting sen to %d \n", sen_id);
  show_oled_msg(OLED_MSG_UPDATE, "waiting sen to %d \n", sen_id);
  /**
   * 进入循环，接收中断消息
   */
  while (sen_id > 0) {
    /**
     * 先读取传感器，查看是不是提前置位
     */
    if (Read_MET_Sensor() == sen_id) {
      printf("sen %d OK!\n", sen_id);
      main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
      return 1;
    }
    /**
     * 如果读取传感器未到达置位，则等待中断消息
     */
    if (xQueueReceive(QUEUE_MET_SEN, &action_sen_id, pdMS_TO_TICKS(2000)) ==
        pdPASS) {
      if (action_sen_id == sen_id) {
        if (Read_MET_Sensor() == sen_id) {
          printf("sen %d OK!\n", sen_id);
          main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
          return 1;
        }
      }
    } else {
      if (Read_MET_Sensor() == sen_id) {
        printf("sen %d OK!\n", sen_id);
        main_sys_status.q_model.sen_status = SENSOR_STATUS_READY;
        return 1;
      } else {
        printf("sen %d timeout\n", sen_id);
        show_oled_msg(OLED_MSG_UPDATE, "sen %d timeout\n", sen_id);
        if (sen_id == 1) {
          main_sys_status.q_model.sen_status = SENSOR_STATUS_T_OFFLINE;
        } else {
          main_sys_status.q_model.sen_status = SENSOR_STATUS_D_OFFLINE;
        }

        return 0;
      }
    }
    if ((tick_s + SYS_INJECT_TIMEOUT) < HAL_GetTick()) {

      printf("sen %d timeout\n", sen_id);
      show_oled_msg(OLED_MSG_UPDATE, "sen %d timeout\n", sen_id);
      if (sen_id == 1) {
        main_sys_status.q_model.sen_status = SENSOR_STATUS_T_OFFLINE;
      } else {
        main_sys_status.q_model.sen_status = SENSOR_STATUS_D_OFFLINE;
      }

      return 0;
    }
  }
  return 0;
}
