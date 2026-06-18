/**
 * @file oled_page.c
 * @brief OLED页面管理源文件
 * @author 梁伟
 * @date 2026-04-20
 */

#include "oled_page.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "msg_queue.h"
#include "oled_task.h"
#include "oled_u8g2.h"
#include "rtc_clock.h"
#include "running_logic.h"
#include "sht40.h"
#include "task.h"
#include <Encoder.h>
#include <string.h>

// 全局菜单实例
Menu g_menu;

/**
 * @brief 显示温湿度页面
 */
void show_temperature_humidity_page(u8g2_t *u8g2)
{

    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "BORCH  LUBE")) / 2, 18, "BORCH  LUBE");

        if (main_sys_status.p_model.status != MOTOR_STATUS_READY)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 12, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 7, 39, "P");
        }
        if (main_sys_status.q_model.motor_status != MOTOR_STATUS_READY)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 32, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 27, 39, "Q");
        }

        if (main_sys_status.q_model.sen_status != SENSOR_STATUS_READY)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 52, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 47, 39, "S");
        }
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        SHT40_Read_Temperature_Humidity(&Temperature, &Humidity);
        char buf[32] = {0};
        float2str(Temperature, 2, buf, 32);

        u8g2_printf(u8g2, 20, 100, "T: %sC", buf);
        memset(buf, 0, sizeof(buf));
        float2str(Humidity, 2, buf, 32);
        u8g2_printf(u8g2, 148, 100, "H: %s%s", buf, "%");
        refresh_rtc_time();
        u8g2_printf(u8g2, 40, 80, "20%02d-%02d-%02d %02d:%02d:%02d", RtcData.Year, RtcData.Month, RtcData.Date, RtcTime.Hours, RtcTime.Minutes, RtcTime.Seconds);

        // 状态栏
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        if (oled_msg.type != OLED_MSG_NULL)
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, oled_msg.data)) / 2, OLED_HEIGHT - 6, oled_msg.data);
        }
        else
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Long press to return")) / 2, OLED_HEIGHT - 6, "Long press to return");
        }

        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

/**
 * @brief 示例菜单项回调函数
 */
void menu_item_callback1(void)
{
    // 显示温湿度页面
    extern u8g2_t g_u8g2;
    show_temperature_humidity_page(&g_u8g2);

    // 等待长按返回
    EncoderState encoder_state;
    uint32_t start_time = xTaskGetTickCount();
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                long_pressed = 1;
            }
        }

        // 定期更新数据
        if (xTaskGetTickCount() - start_time > 500)
        {
            start_time = xTaskGetTickCount();
            show_temperature_humidity_page(&g_u8g2);
        }
    }
}

void menu_item_callback2(void)
{
    // 系统设置页面
    extern u8g2_t g_u8g2;

    u8g2_FirstPage(&g_u8g2);
    do
    {
        u8g2_DrawBox(&g_u8g2, 0, 0, OLED_WIDTH, 24);
        u8g2_SetFont(&g_u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(&g_u8g2, 0);
        u8g2_DrawStr(&g_u8g2, (OLED_WIDTH - u8g2_GetStrWidth(&g_u8g2, "System Settings")) / 2, 16, "System Settings");
        u8g2_SetDrawColor(&g_u8g2, 1);

        u8g2_SetFont(&g_u8g2, u8g2_font_9x15_tf);
        u8g2_DrawStr(&g_u8g2, 50, 60, "Settings Page");
        u8g2_DrawStr(&g_u8g2, 50, 80, "Under Development");

        u8g2_DrawBox(&g_u8g2, 0, OLED_HEIGHT - 24, OLED_WIDTH, 24);
        u8g2_SetFont(&g_u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(&g_u8g2, 0);
        u8g2_DrawStr(&g_u8g2, (OLED_WIDTH - u8g2_GetStrWidth(&g_u8g2, "Press to return")) / 2, OLED_HEIGHT - 12, "Press to return");
        u8g2_SetDrawColor(&g_u8g2, 1);

    } while (u8g2_NextPage(&g_u8g2));

    // 等待按键返回
    EncoderState encoder_state;
    xQueueReceive(QUEUE_KEY, &encoder_state, portMAX_DELAY);
}

/**
 * @brief 显示P模型初始化页面
 * @param u8g2 用于绘制的u8g2实例指针
 * @param angle 当前角度值
 * @param msg 状态消息字符串
 */
void show_p_model_init_page(u8g2_t *u8g2, uint16_t angle, char *msg)
{
    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "P Model Init")) / 2, 16, "P Model Init");
        u8g2_SetDrawColor(u8g2, 1);

        // 绘制P模型状态

        /* 绘制中心圆圈 */
        u8g2_DrawCircle(u8g2, OLED_WIDTH / 4, OLED_HEIGHT / 2, 30, U8G2_DRAW_ALL);

        /* 绘制旋转的线段 */
        float rad = (float)angle * 3.14159 / 180;
        int x = OLED_WIDTH / 4 + (int)(35 * cos(rad));
        int y = OLED_HEIGHT / 2 + (int)(35 * sin(rad));
        u8g2_DrawLine(u8g2, OLED_WIDTH / 4, OLED_HEIGHT / 2, x, y);
        /* 绘制P模型状态文本 */
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        u8g2_printf(u8g2, 128, 48, "Angle: %d", angle);
        u8g2_printf(u8g2, 128, 80, msg);

        // 状态栏
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        if (oled_msg.type != OLED_MSG_NULL)
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, oled_msg.data)) / 2, OLED_HEIGHT - 6, oled_msg.data);
        }
        else
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Long press to return")) / 2, OLED_HEIGHT - 6, "Long press to return");
        }

        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void menu_item_callback3(void)
{
    // 停止系统运行，进入设置模式
    main_sys_status.running_mode = SYS_MODE_SETTING;
    uint16_t angle = 1;
    // P模型初始化页面
    extern u8g2_t g_u8g2;
    show_p_model_init_page(&g_u8g2, angle, "Init ...");
    // 读取当前P电机角度
    angle = zdt_get_motor_loc_abs(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE);
    // 等待长按返回
    EncoderState encoder_state;
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {

            switch (encoder_state)
            {
            case ENCODER_PUTH_LONG:

                long_pressed = 1;
                // 返回前设置系统运行模式为SYS_MODE_STARTING
                main_sys_status.running_mode = SYS_MODE_STARTING;
                break;
            case ENCODER_PUTH:
                if (zdt_set_motor_reset_zero(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg) == 1)
                {
                    angle = zdt_get_motor_loc_abs(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE);
                    show_p_model_init_page(&g_u8g2, angle, "Save!");
                }
                else
                {
                    show_p_model_init_page(&g_u8g2, angle, "Failed!");
                }

                break;
            case ENCODER_CW:
                zdt_run_motor_loc_rel(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE, 0x01, MOTOR_RUN_CW);
                angle = zdt_get_motor_loc_abs(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE);
                show_p_model_init_page(&g_u8g2, angle, "Ready!");
                break;
            case ENCODER_CCW:
                zdt_run_motor_loc_rel(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE, 0x01, MOTOR_RUN_CCW);
                angle = zdt_get_motor_loc_abs(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE);
                show_p_model_init_page(&g_u8g2, angle, "Ready!");
                break;

            default:
                break;
            }
        }
    }
}

void show_q_model_init_page(u8g2_t *u8g2, uint16_t angle, char *msg)
{
    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Q Model Init")) / 2, 16, "Q Model Init");
        u8g2_SetDrawColor(u8g2, 1);

        // 绘制Q模型状态

        /* 绘制中心圆圈 */
        u8g2_DrawCircle(u8g2, OLED_WIDTH / 4, OLED_HEIGHT / 2, 30, U8G2_DRAW_ALL);

        /* 绘制旋转的线段 */
        float rad = (float)angle * 3.14159 / 180;
        int x = OLED_WIDTH / 4 + (int)(35 * cos(rad));
        int y = OLED_HEIGHT / 2 + (int)(35 * sin(rad));
        u8g2_DrawLine(u8g2, OLED_WIDTH / 4, OLED_HEIGHT / 2, x, y);
        /* 绘制P模型状态文本 */
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        u8g2_printf(u8g2, 128, 48, "Angle: %d", angle);
        u8g2_printf(u8g2, 128, 80, msg);

        // 状态栏
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        if (oled_msg.type != OLED_MSG_NULL)
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, oled_msg.data)) / 2, OLED_HEIGHT - 6, oled_msg.data);
        }
        else
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Long press to return")) / 2, OLED_HEIGHT - 6, "Long press to return");
        }

        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void menu_item_callback4(void)
{
    uint16_t angle = 1;
    // 停止系统运行，进入设置模式
    main_sys_status.running_mode = SYS_MODE_SETTING;

    // Q模型初始化页面
    extern u8g2_t g_u8g2;

    show_q_model_init_page(&g_u8g2, angle, "Init ...");
    // 读取当前Q电机角度
    angle = zdt_get_motor_loc_abs(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE);

    // 等待长按返回
    EncoderState encoder_state;
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {

            switch (encoder_state)
            {
            case ENCODER_PUTH_LONG:
                long_pressed = 1;
                // 返回前设置系统运行模式为SYS_MODE_STARTING
                main_sys_status.running_mode = SYS_MODE_STARTING;
                break;
            case ENCODER_PUTH:
                if (zdt_set_motor_reset_zero(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg) == 1)
                {
                    angle = zdt_get_motor_loc_abs(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE);
                    show_q_model_init_page(&g_u8g2, angle, "Save!");
                }
                else
                {
                    show_q_model_init_page(&g_u8g2, angle, "Failed!");
                }
                break;
            case ENCODER_CW:
                zdt_run_motor_loc_rel(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE, 0x01, MOTOR_RUN_CW);
                angle = zdt_get_motor_loc_abs(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE);
                show_q_model_init_page(&g_u8g2, angle, "Ready!");
                break;
            case ENCODER_CCW:
                zdt_run_motor_loc_rel(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE, 0x01, MOTOR_RUN_CCW);
                angle = zdt_get_motor_loc_abs(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE);
                show_q_model_init_page(&g_u8g2, angle, "Ready!");
                show_q_model_init_page(&g_u8g2, angle, "Ready!");
                break;

            default:
                break;
            }
        }
    }
}

void show_test_running_page(u8g2_t *u8g2, char *inj_num, char *msg)
{
    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Test Running")) / 2, 16, "Test Running");
        u8g2_SetDrawColor(u8g2, 1);

        // 绘制Q模型状态

        /* 绘制中心圆圈 */

        /* 绘制P模型状态文本 */
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        u8g2_printf(u8g2, 40, 48, "injector: %s", inj_num);
        u8g2_printf(u8g2, 40, 80, "status: %s", msg);

        // 状态栏
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        if (oled_msg.type != OLED_MSG_NULL)
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, oled_msg.data)) / 2, OLED_HEIGHT - 6, oled_msg.data);
        }
        else
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Long press to return")) / 2, OLED_HEIGHT - 6, "Long press to return");
        }

        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void menu_item_callback5(void)
{
    // 执行标记
    uint8_t running_tag = 0;
    uint8_t injector_num = 0;
    uint8_t inj_buf = 1;
    char running_msg[32] = {0};
    // 检查当前阀体状态，是否具备运行条件
    if (main_sys_status.running_mode == SYS_MODE_READY)
    {
        memset(running_msg, '\0', sizeof(running_msg));
        strncpy(running_msg, "Ready !", sizeof(running_msg) - 1);
        main_sys_status.running_mode = SYS_MODE_MONITOR;
    }
    else
    {
        memset(running_msg, '\0', sizeof(running_msg));
        strncpy(running_msg, "Not Ready !", sizeof(running_msg) - 1);
    }
    // 测试运行页面
    extern u8g2_t g_u8g2;

    EncoderState encoder_state;
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {

            switch (encoder_state)
            {
            case ENCODER_PUTH_LONG:
                long_pressed = 1;
                // 返回前设置系统运行模式为SYS_MODE_STARTING
                main_sys_status.running_mode = SYS_MODE_STARTING;
                break;
            case ENCODER_PUTH:
                if (running_tag > 0)
                {
                    running_tag = 0;
                    memset(running_msg, '\0', sizeof(running_msg));
                    strncpy(running_msg, "Stopped !", sizeof(running_msg) - 1);
                }
                else
                {

                    if (main_sys_status.running_mode == SYS_MODE_MONITOR)
                    {

                        running_tag = 1;
                        memset(running_msg, '\0', sizeof(running_msg));
                        strncpy(running_msg, "Running !", sizeof(running_msg) - 1);
                        // 任务列表中没有任务的时候再添加任务
                        if (main_sys_status.is_Busy == 0 && task_object_list[4].task_status == 0 && running_tag == 1)
                        {
                            if (injector_num == 0)
                            {
                                if (inj_buf > injector_count)
                                {
                                    inj_buf = 1;
                                    task_object_list[4].inject_id = inj_buf;
                                    task_object_list[4].task_status = 1;
                                    task_object_list[4].val = 3;
                                    inj_buf++;
                                }
                                else
                                {
                                    task_object_list[4].inject_id = inj_buf;
                                    task_object_list[4].task_status = 1;
                                    task_object_list[4].val = 3;
                                    inj_buf++;
                                }
                            }
                            else
                            {
                                task_object_list[4].inject_id = injector_num;
                                task_object_list[4].task_status = 1;
                                task_object_list[4].val = 3;
                            }
                        }
                    }
                    else
                    {
                        memset(running_msg, '\0', sizeof(running_msg));
                        strncpy(running_msg, "Not Ready !", sizeof(running_msg) - 1);
                    }
                }

                break;
            case ENCODER_CW:
                if (injector_num > 7)
                {
                    injector_num = 0;
                }
                else
                {
                    injector_num++;
                }
                break;
            case ENCODER_CCW:
                if (injector_num < 1)
                {
                    injector_num = 8;
                }
                else
                {
                    injector_num--;
                }
                break;

            default:

                break;
            }
        }
        if (main_sys_status.running_mode == SYS_MODE_MONITOR && main_sys_status.is_Busy == 0 && task_object_list[4].task_status == 0 && running_tag == 1)
        {
            if (injector_num == 0)
            {
                if (inj_buf > injector_count)
                {
                    inj_buf = 1;
                    task_object_list[4].inject_id = inj_buf;
                    task_object_list[4].task_status = 1;
                    task_object_list[4].val = 3;
                    inj_buf++;
                }
                else
                {
                    task_object_list[4].inject_id = inj_buf;
                    task_object_list[4].task_status = 1;
                    task_object_list[4].val = 3;
                    inj_buf++;
                }
            }
            else
            {
                task_object_list[4].inject_id = injector_num;
                task_object_list[4].task_status = 1;
                task_object_list[4].val = 3;
            }
        }
        if (main_sys_status.running_mode == SYS_MODE_READY)
        {
            main_sys_status.running_mode = SYS_MODE_MONITOR;
            memset(running_msg, '\0', sizeof(running_msg));
            strncpy(running_msg, "Ready !", sizeof(running_msg) - 1);
        }
        show_test_running_page(&g_u8g2, injector_num == 0 ? "auto" : (char[2]){(char)(injector_num + '0'), '\0'}, running_msg);
    }
}

void menu_item_callback6(void)
{
    // 关于设备页面
    extern u8g2_t g_u8g2;

    u8g2_FirstPage(&g_u8g2);
    do
    {
        u8g2_DrawBox(&g_u8g2, 0, 0, OLED_WIDTH, 24);
        u8g2_SetFont(&g_u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(&g_u8g2, 0);
        u8g2_DrawStr(&g_u8g2, (OLED_WIDTH - u8g2_GetStrWidth(&g_u8g2, "About Device")) / 2, 16, "About Device");
        u8g2_SetDrawColor(&g_u8g2, 1);

        u8g2_SetFont(&g_u8g2, u8g2_font_9x15_tf);
        u8g2_DrawStr(&g_u8g2, 50, 50, "Device: BORCH LUBE");
        u8g2_DrawStr(&g_u8g2, 50, 70, "Version: V1.0.0");
        u8g2_DrawStr(&g_u8g2, 50, 90, "Date: 2026-01-30");

        u8g2_DrawBox(&g_u8g2, 0, OLED_HEIGHT - 24, OLED_WIDTH, 24);
        u8g2_SetFont(&g_u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(&g_u8g2, 0);
        u8g2_DrawStr(&g_u8g2, (OLED_WIDTH - u8g2_GetStrWidth(&g_u8g2, "Press to return")) / 2, OLED_HEIGHT - 12, "Press to return");
        u8g2_SetDrawColor(&g_u8g2, 1);

    } while (u8g2_NextPage(&g_u8g2));

    // 等待按键返回
    EncoderState encoder_state;
    xQueueReceive(QUEUE_KEY, &encoder_state, portMAX_DELAY);
}

void menu_item_callback7(void)
{
    // 重启设备
    extern u8g2_t g_u8g2;

    u8g2_FirstPage(&g_u8g2);
    do
    {
        u8g2_DrawBox(&g_u8g2, 0, 0, OLED_WIDTH, 24);
        u8g2_SetFont(&g_u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(&g_u8g2, 0);
        u8g2_DrawStr(&g_u8g2, (OLED_WIDTH - u8g2_GetStrWidth(&g_u8g2, "Reboot")) / 2, 16, "Reboot");
        u8g2_SetDrawColor(&g_u8g2, 1);

        u8g2_SetFont(&g_u8g2, u8g2_font_ncenB18_tr);
        u8g2_DrawStr(&g_u8g2, 50, 70, "Rebooting...");

        // 绘制重启动画
        static uint8_t reboot_step = 0;
        reboot_step = (reboot_step + 1) % 4;
        for (uint8_t i = 0; i < reboot_step; i++)
        {
            u8g2_DrawCircle(&g_u8g2, OLED_WIDTH - 60, 70, 5 + i * 3, U8G2_DRAW_ALL);
        }

    } while (u8g2_NextPage(&g_u8g2));

    // 延时后重启（实际项目中可调用系统重启函数）
    vTaskDelay(pdMS_TO_TICKS(2000));
}

/**
 * @brief 单元测试运行页面
 */
void unit_test_running_page(u8g2_t *u8g2, uint16_t p_angle, uint16_t q_angle, char sen_top, char sen_bottom,uint16_t running_tag )
{
    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Unit Test")) / 2, 16, "Unit Test");
        u8g2_SetDrawColor(u8g2, 1);

        // 绘制Q模型状态

        /* 绘制中心圆圈 */
        u8g2_DrawDisc(u8g2, running_tag, 100, 4, U8G2_DRAW_ALL);

        /* 绘制P模型状态文本 */
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        u8g2_printf(u8g2, 20, 48, "p_ang:%d", p_angle);
        u8g2_printf(u8g2, 20, 80, "q_angle:%d", q_angle);
        u8g2_printf(u8g2, 148, 48, "sen_t: %d", sen_top);
        u8g2_printf(u8g2, 148, 80, "sen_b: %d", sen_bottom);
        // 状态栏
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        if (oled_msg.type != OLED_MSG_NULL)
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, oled_msg.data)) / 2, OLED_HEIGHT - 6, oled_msg.data);
        }
        else
        {
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Long press to return")) / 2, OLED_HEIGHT - 6, "Long press to return");
        }

        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}
/**
 * @brief 单元测试运行页面
 */
void menu_item_unit_test(void)
{
   // 执行标记

    uint8_t p_step = 0;
    uint8_t q_step = 0;


    uint16_t p_angle = 0;
    uint16_t q_angle = 0;

    uint8_t sen_top = 0;
    uint8_t sen_bottom = 0;

    uint16_t running_tag = 0;

    // 无条件进入测试模式
  
    main_sys_status.running_mode = SYS_MODE_SETTING;
   
    // 测试运行页面
    extern u8g2_t g_u8g2;

    EncoderState encoder_state;
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(200)) == pdPASS)
        {

            switch (encoder_state)
            {
            case ENCODER_PUTH_LONG:
                long_pressed = 1;
                // 返回前设置系统运行模式为SYS_MODE_STARTING
                main_sys_status.running_mode = SYS_MODE_STARTING;
                break;
            case ENCODER_PUTH:
                if(p_step>7){
                    p_step=0;
                }
                p_motor_run_point(motor_loc_list[p_step]);
                p_step++;              
                break;
            case ENCODER_CW:
                if (q_step > 7)
                {
                    q_step = 0;
                }
                q_motor_run_point(motor_loc_list[q_step]);
                q_step++;              
                break;
            case ENCODER_CCW:
                if (q_step < 1)
                {
                    q_step = 8;
                }
                q_motor_run_point(motor_loc_list[q_step]);
                q_step--;
                break;

            default:

                break;
            }
        }
        q_angle = zdt_get_motor_loc_abs(Q_MOTOR_ADDR, Q_MOTOR_IF, zdt_q_msg, Q_MOTOR_RATE);
        p_angle = zdt_get_motor_loc_abs(P_MOTOR_ADDR, P_MOTOR_IF, zdt_p_msg, P_MOTOR_RATE);
        sen_top = READ_MET_SEN_1;
        sen_bottom = READ_MET_SEN_2;
        if(running_tag>255){
            running_tag=0;
        }
        running_tag++;

        unit_test_running_page(&g_u8g2, p_angle, q_angle, sen_top, sen_bottom,running_tag);
    }
}

// 定义菜单项
MenuItem g_menu_items[] = {
    {"Home Page", menu_item_callback1},
    {"System Settings", menu_item_callback2},
    {"P Model Init", menu_item_callback3},
    {"Q Model Init", menu_item_callback4},
    {"Test Running", menu_item_callback5},
    {"Unit Test", menu_item_unit_test},
    {"About Device", menu_item_callback6},
    {"Reboot", menu_item_callback7}};

void show_oled_boot_page(u8g2_t *u8g2)
{
    // 进度条数值
    float progress = 0;
    while (progress < 1.0f)
    {

        // 1. 开始绘制（u8g2采用双缓冲/单缓冲，需先调用u8g2_FirstPage()进入绘制循环）
        u8g2_FirstPage(u8g2);
        do
        {
            u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);                                                    // 设置字体
            u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, DEVICE_LOGO)) / 2, 45, DEVICE_LOGO); // 绘制设备名称

            u8g2_DrawFrame(u8g2, 10, 90, OLED_WIDTH - 20, 20); // 绘制边框

            u8g2_DrawBox(u8g2, 10, 90, (OLED_WIDTH - 20) * progress, 20); // 绘制进度条

        } while (u8g2_NextPage(u8g2)); // 结束绘制，刷新到屏幕
        // 更新进度
        progress += 0.02f;
        // 延时50ms
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 2. 动画结束后，停留2秒
    vTaskDelay(pdMS_TO_TICKS(500));
    // 3. 清屏，进入主界面
    u8g2_ClearDisplay(u8g2);
}

/**
 * @brief 初始化菜单
 * @param menu 菜单指针
 * @param items 菜单项数组
 * @param item_total 菜单项总数
 */
void menu_init(Menu *menu, MenuItem *items, uint8_t item_total)
{
    menu->items = items;
    menu->item_total = item_total;
    menu->cur_index = 0;
    menu->top_index = 0;
}

/**
 * @brief 处理菜单事件
 * @param event 事件类型
 */
void menu_handle_event(MenuEventType event)
{
    switch (event)
    {
    case MENU_EVENT_UP:
        if (g_menu.cur_index > 0)
        {
            g_menu.cur_index--;
            // 如果当前项移出可见区域，调整top_index
            if (g_menu.cur_index < g_menu.top_index)
            {
                g_menu.top_index--;
            }
        }
        break;

    case MENU_EVENT_DOWN:
        if (g_menu.cur_index < g_menu.item_total - 1)
        {
            g_menu.cur_index++;
            // 如果当前项移出可见区域，调整top_index
            if (g_menu.cur_index >= g_menu.top_index + MENU_VISIBLE_MAX)
            {
                g_menu.top_index++;
            }
        }
        break;

    case MENU_EVENT_ENTER:
        // 执行当前选中项的回调函数
        if (g_menu.items[g_menu.cur_index].callback != NULL)
        {
            g_menu.items[g_menu.cur_index].callback();
        }
        break;

    case MENU_EVENT_BACK:
        // 返回上一级菜单（如果有）
        // 这里可以添加多级菜单的处理逻辑
        break;

    default:
        break;
    }
}

/**
 * @brief 显示菜单页面
 * @param u8g2
 */
void show_oled_menu_page(u8g2_t *u8g2)
{
    u8g2_FirstPage(u8g2);
    do
    {
        // -------------------------- 第一段：标题栏（高亮，优雅醒目） --------------------------
        // 1. 标题栏背景（实心框，占据顶部22像素，形成视觉分区）
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);

        // 2. 标题文字（居中、加粗、反色显示，优雅突出）
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr); // 加粗字体，提升标题质感
        u8g2_SetDrawColor(u8g2, 0);               // 反色：黑底白字→白底黑字（标题栏背景为白，文字为黑）
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, MENU_TITLE)) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, MENU_TITLE); // 标题垂直居中（24像素高度，y=16最佳）
        u8g2_SetDrawColor(u8g2, 1);                  // 恢复默认绘制颜色

        // -------------------------- 第二段：菜单列表（核心，清晰规整） --------------------------
        // 1. 菜单列表背景（浅淡分隔，与标题栏/状态栏区分）
        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44); // 空心框，形成列表区域

        // 2. 绘制可见菜单项（循环绘制，支持滚动）
        uint16_t menu_y = 32;                  // 菜单列表起始y坐标（标题栏下方8像素，优雅间距）
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf); // 中等字体，清晰易读，适合菜单

        for (uint8_t i = g_menu.top_index; i < g_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            // 超出菜单总数，停止绘制
            if (i >= g_menu.item_total)
                break;

            // 选中项：突出显示（实心背景+加粗文字，优雅醒目）
            if (i == g_menu.cur_index)
            {
                // 选中项背景（占据整行，高度与菜单项一致，无违和感）
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);

                // 选中项文字（反色显示，与背景形成对比）
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, g_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, g_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            // 未选中项：正常显示（透明背景，清晰整洁）
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, g_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, g_menu.items[i].item_name);
            }

            // 下一个菜单项y坐标偏移
            menu_y += MENU_ITEM_H;
        }

        // -------------------------- 第三段：状态栏（浅淡，辅助信息） --------------------------
        // 1. 状态栏背景（与标题栏呼应，占据底部24像素）
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);

        // 2. 状态栏文字（居中、小字体、反色显示，不抢焦点）
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf); // 小字体，辅助信息，不干扰主界面
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, MENU_STATUS)) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, MENU_STATUS);
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}
