/**
 * @file oled_page.c
 * @brief OLED页面管理源文件
 * @author 任晓宇
 * @date 2026-08-11
 */

#include "oled_page.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "main.h"
#include "oled_task.h"
#include "oled_u8g2.h"
#include "rtc_clock.h"
#include "sht40.h"
#include "task.h"
#include <Encoder.h>
#include <stdint.h>
#include <string.h>
#include "system_service.h"
#include "syncif.h"
#include "msg_queue.h"
#include "ADC_collect.h"
#include "main_logic.h"
#include "net_comm_task.h"
#include "modbus_master.h"

// 全局菜单实例
void show_oled_alarm_record_page(u8g2_t *u8g2);
static void sub_menu_handle_event(Menu *menu, MenuEventType event, uint8_t *exit_flag);
void param_settings_callback(void);
void comm_settings_callback(void);
void system_info_callback(void);
void debug_mode_callback(void);
void alarm_record_callback(void);
void show_oled_param_settings_page(u8g2_t *u8g2);
void show_oled_comm_settings_page(u8g2_t *u8g2);
void show_oled_system_info_page(u8g2_t *u8g2);
void show_oled_debug_mode_page(u8g2_t *u8g2);
void show_oled_alarm_record_page(u8g2_t *u8g2);

// 压力相关参数回调声明
void param_ref_press_callback(void);
void param_max_press_callback(void);
void param_motor_max_rpm_callback(void);
void param_motor_min_rpm_callback(void);
void param_temp_thres_callback(void);
void param_level_thres_callback(void);
void param_pid_callback(void);

// 通讯设置回调声明
void comm_modbus_addr_callback(void);
void comm_baudrate_callback(void);
void comm_uart_param_callback(void);
void comm_ip_callback(void);
void comm_netmask_callback(void);
void comm_gateway_callback(void);

// 系统信息回调声明
void info_fw_ver_callback(void);
void info_hw_ver_callback(void);
void info_runtime_callback(void);
void info_sensor_raw_callback(void);

// Debug调试菜单回调声明
void debug_manual_ctrl_callback(void);
void debug_pid_tune_callback(void);
void debug_io_monitor_callback(void);
void debug_modbus_test_callback(void);
void debug_adc_watch_callback(void);

// 报警记录菜单回调声明
void alarm_history_list_callback(void);
void alarm_clear_all_callback(void);



//系统设置次级菜单项
Menu system_settings_menu;
MenuItem system_settings_menu_items[] = {
    {"Serial Port", serial_port_settings_callback},
    {"Restore Factory", restore_factory_settings_callback},
    {"Back", NULL}}; // 返回上一级菜单项，回调函数为NULL，特殊处理

// 定义菜单项
// MenuItem g_menu_items[] = {
//     {"Home Page", home_page_callback},
//     {"System Settings", system_settings_callback},
//     {"P Model Init", p_model_init_callback},
//     {"Q Model Init", q_model_init_callback},
//     {"Test Running", test_running_callback},
//     {"Unit Test", menu_item_unit_test},
//     {"About Device", about_device_callback},
//     {"Reboot", reboot_callback}};

// ====================== 一级主菜单 ======================
// 原顶层主菜单 g_menu_items 替换为新7项主菜单
Menu g_menu;
MenuItem g_menu_items[] = {
    {"Home Page", home_page_callback},                      // 0.首页
    {"Param Settings", param_settings_callback},            // 1.参数设置
    {"Comm Settings", comm_settings_callback},              // 2.通讯设置
    {"System Info", system_info_callback},                  // 3.系统信息
    {"Debug Mode", debug_mode_callback},                    // 4.调试模式
    {"Alarm Record", alarm_record_callback},                // 5.报警记录
    {"Restore Factory", restore_factory_settings_callback}  // 6.恢复出厂
};

// ====================== 二级子菜单1：参数设置 ======================
Menu param_settings_menu;
MenuItem param_settings_items[] = {
    {"Ref Pressure", param_ref_press_callback},         //基准压力
    {"Max Pressure", param_max_press_callback},         //最高压力
    {"Motor Max RPM", param_motor_max_rpm_callback},    //电机最高转速
    {"Motor Min RPM", param_motor_min_rpm_callback},    //电机最低转速
    {"Temp Alarm Thres", param_temp_thres_callback},    //温度报警/停机阈值
    {"Liquid Level Thres", param_level_thres_callback}, //液位报警/停机阈值
    {"PID P/I/D", param_pid_callback},                  //PID 参数（P / I / D）
    {"Back", NULL}
};

// ====================== 二级子菜单2：通讯设置 ======================
Menu comm_settings_menu;
MenuItem comm_settings_items[] = {
    {"Modbus Slave Addr", comm_modbus_addr_callback},   //Modbus从站地址
    {"UART Baudrate", comm_baudrate_callback},          //UART波特率
    {"UART Parity/Data", comm_uart_param_callback},     //UART数据位/停止位/校验位
    {"ETH IP", comm_ip_callback},                       //ETH IP地址
    {"ETH Netmask", comm_netmask_callback},             //ETH 子网掩码
    {"ETH Gateway", comm_gateway_callback},             //ETH 网关地址
    {"Back", NULL}
};

// ====================== 二级子菜单3：系统信息 ======================
Menu system_info_menu;
MenuItem system_info_items[] = {
    {"Firmware Ver", info_fw_ver_callback},         //固件版本
    {"Hardware Ver", info_hw_ver_callback},         //硬件版本
    {"Runtime Stat", info_runtime_callback},        //运行时间统计
    {"Sensor Raw ADC", info_sensor_raw_callback},   //传感器原始ADC值
    {"Back", NULL}
};

// ====================== 二级子菜单4：调试模式 ======================
Menu debug_mode_menu;
MenuItem debug_mode_items[] = {
    {"Manual Ctrl Motor/Valve", debug_manual_ctrl_callback},//手动控制电机/阀门
    {"PID Auto Tune", debug_pid_tune_callback},             //PID自动调参
    {"IO Monitor", debug_io_monitor_callback},              //IO监控
    {"Modbus Test", debug_modbus_test_callback},            //Modbus测试
    {"ADC Raw Watch", debug_adc_watch_callback},            //ADC原始值监控
    {"Back", NULL}
};

// ====================== 二级子菜单5：报警记录 ======================
Menu alarm_record_menu;
MenuItem alarm_record_items[] = {
    {"History Alarm List", alarm_history_list_callback},//历史报警记录
    {"Clear All Alarm", alarm_clear_all_callback},      //清除所有报警记录
    {"Back", NULL}
};

// ====================== 菜单超时配置 ======================

TickType_t g_menu_last_op_tick = 0;



/**
 * @brief 通用二级菜单事件分发处理
 * @param menu 二级菜单实例指针
 * @param event 菜单上下/确认/返回事件
 * @param exit_flag 退出二级菜单标记指针
 */
static void sub_menu_handle_event(Menu *menu, MenuEventType event, uint8_t *exit_flag)
{
    switch (event)
    {
    case MENU_EVENT_UP:
        if (menu->cur_index > 0)
        {
            menu->cur_index--;
            if (menu->cur_index < menu->top_index)
                menu->top_index--;
        }
        break;
    case MENU_EVENT_DOWN:
        if (menu->cur_index < menu->item_total - 1)
        {
            menu->cur_index++;
            if (menu->cur_index >= menu->top_index + MENU_VISIBLE_MAX)
                menu->top_index++;
        }
        break;
    case MENU_EVENT_ENTER:
        // 选中项存在回调则执行，无回调代表Back返回
        if (menu->items[menu->cur_index].callback != NULL)
        {
            menu->items[menu->cur_index].callback();
        }
        else
        {
            *exit_flag = 0;
        }
        break;
    case MENU_EVENT_BACK:
        *exit_flag = 0;
        break;
    default:
        break;
    }
}

// ====================== 二级菜单回调函数（统一超时逻辑） ======================
void param_settings_callback(void)
{
    menu_init(&param_settings_menu, param_settings_items, sizeof(param_settings_items)/sizeof(MenuItem));
    show_oled_param_settings_page(&g_u8g2);
    menu_reset_idle_timer();

    EncoderState encoder_state;
    uint8_t in_sub_menu = 1;
    while (in_sub_menu)
    {
        // 20s无操作超时检测
        if (menu_is_idle_timeout())
        {
            in_sub_menu = 0;
            break;
        }
        // 等待编码器事件100ms，保证超时逻辑轮询
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
            case ENCODER_CW:
                sub_menu_handle_event(&param_settings_menu, MENU_EVENT_DOWN, &in_sub_menu);
                show_oled_param_settings_page(&g_u8g2);
                break;
            case ENCODER_CCW:
                sub_menu_handle_event(&param_settings_menu, MENU_EVENT_UP, &in_sub_menu);
                show_oled_param_settings_page(&g_u8g2);
                break;
            case ENCODER_PUTH:
                sub_menu_handle_event(&param_settings_menu, MENU_EVENT_ENTER, &in_sub_menu);
                if(in_sub_menu) show_oled_param_settings_page(&g_u8g2);
                break;
            case ENCODER_PUTH_LONG:
                sub_menu_handle_event(&param_settings_menu, MENU_EVENT_BACK, &in_sub_menu);
                show_oled_param_settings_page(&g_u8g2);
                break;
            default: break;
            }
        }
    }
    // 超时则跳转首页；长按退出直接return，外层主循环自动渲染一级主菜单
    if(menu_is_idle_timeout())
    {
        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        home_page_callback();
    }
}

void comm_settings_callback(void)
{
    menu_init(&comm_settings_menu, comm_settings_items, sizeof(comm_settings_items)/sizeof(MenuItem));
    show_oled_comm_settings_page(&g_u8g2);
    menu_reset_idle_timer();

    EncoderState encoder_state;
    uint8_t in_sub_menu = 1;
    while (in_sub_menu)
    {
        if (menu_is_idle_timeout())
        {
            in_sub_menu = 0;
            break;
        }
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
            case ENCODER_CW:
                sub_menu_handle_event(&comm_settings_menu, MENU_EVENT_DOWN, &in_sub_menu);
                show_oled_comm_settings_page(&g_u8g2);
                break;
            case ENCODER_CCW:
                sub_menu_handle_event(&comm_settings_menu, MENU_EVENT_UP, &in_sub_menu);
                show_oled_comm_settings_page(&g_u8g2);
                break;
            case ENCODER_PUTH:
                sub_menu_handle_event(&comm_settings_menu, MENU_EVENT_ENTER, &in_sub_menu);
                if(in_sub_menu) show_oled_comm_settings_page(&g_u8g2);
                break;
            case ENCODER_PUTH_LONG:
                sub_menu_handle_event(&comm_settings_menu, MENU_EVENT_BACK, &in_sub_menu);
                show_oled_comm_settings_page(&g_u8g2);
                break;
            default: break;
            }
        }
    }
    if(menu_is_idle_timeout())
    {
        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        home_page_callback();
    }
}

void system_info_callback(void)
{
    menu_init(&system_info_menu, system_info_items, sizeof(system_info_items)/sizeof(MenuItem));
    show_oled_system_info_page(&g_u8g2);
    menu_reset_idle_timer();

    EncoderState encoder_state;
    uint8_t in_sub_menu = 1;
    while (in_sub_menu)
    {
        if (menu_is_idle_timeout())
        {
            in_sub_menu = 0;
            break;
        }
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
            case ENCODER_CW:
                sub_menu_handle_event(&system_info_menu, MENU_EVENT_DOWN, &in_sub_menu);
                show_oled_system_info_page(&g_u8g2);
                break;
            case ENCODER_CCW:
                sub_menu_handle_event(&system_info_menu, MENU_EVENT_UP, &in_sub_menu);
                show_oled_system_info_page(&g_u8g2);
                break;
            case ENCODER_PUTH:
                sub_menu_handle_event(&system_info_menu, MENU_EVENT_ENTER, &in_sub_menu);
                if(in_sub_menu) show_oled_system_info_page(&g_u8g2);
                break;
            case ENCODER_PUTH_LONG:
                sub_menu_handle_event(&system_info_menu, MENU_EVENT_BACK, &in_sub_menu);
                show_oled_system_info_page(&g_u8g2);
                break;
            default: break;
            }
        }
    }
    if(menu_is_idle_timeout())
    {
        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        home_page_callback();
    }
}

void debug_mode_callback(void)
{
    menu_init(&debug_mode_menu, debug_mode_items, sizeof(debug_mode_items)/sizeof(MenuItem));
    show_oled_debug_mode_page(&g_u8g2);
    menu_reset_idle_timer();

    EncoderState encoder_state;
    uint8_t in_sub_menu = 1;
    while (in_sub_menu)
    {
        if (menu_is_idle_timeout())
        {
            in_sub_menu = 0;
            break;
        }
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
            case ENCODER_CW:
                sub_menu_handle_event(&debug_mode_menu, MENU_EVENT_DOWN, &in_sub_menu);
                show_oled_debug_mode_page(&g_u8g2);
                break;
            case ENCODER_CCW:
                sub_menu_handle_event(&debug_mode_menu, MENU_EVENT_UP, &in_sub_menu);
                show_oled_debug_mode_page(&g_u8g2);
                break;
            case ENCODER_PUTH:
                sub_menu_handle_event(&debug_mode_menu, MENU_EVENT_ENTER, &in_sub_menu);
                if(in_sub_menu) show_oled_debug_mode_page(&g_u8g2);
                break;
            case ENCODER_PUTH_LONG:
                sub_menu_handle_event(&debug_mode_menu, MENU_EVENT_BACK, &in_sub_menu);
                show_oled_debug_mode_page(&g_u8g2);
                break;
            default: break;
            }
        }
    }
    if(menu_is_idle_timeout())
    {
        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        home_page_callback();
    }
}

void alarm_record_callback(void)
{
    menu_init(&alarm_record_menu, alarm_record_items, sizeof(alarm_record_items)/sizeof(MenuItem));
    show_oled_alarm_record_page(&g_u8g2);
    menu_reset_idle_timer();

    EncoderState encoder_state;
    uint8_t in_sub_menu = 1;
    while (in_sub_menu)
    {
        if (menu_is_idle_timeout())
        {
            in_sub_menu = 0;
            break;
        }
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
            case ENCODER_CW:
                sub_menu_handle_event(&alarm_record_menu, MENU_EVENT_DOWN, &in_sub_menu);
                show_oled_alarm_record_page(&g_u8g2);
                break;
            case ENCODER_CCW:
                sub_menu_handle_event(&alarm_record_menu, MENU_EVENT_UP, &in_sub_menu);
                show_oled_alarm_record_page(&g_u8g2);
                break;
            case ENCODER_PUTH:
                sub_menu_handle_event(&alarm_record_menu, MENU_EVENT_ENTER, &in_sub_menu);
                if(in_sub_menu) show_oled_alarm_record_page(&g_u8g2);
                break;
            case ENCODER_PUTH_LONG:
                sub_menu_handle_event(&alarm_record_menu, MENU_EVENT_BACK, &in_sub_menu);
                show_oled_alarm_record_page(&g_u8g2);
                break;
            default: break;
            }
        }
    }
    if(menu_is_idle_timeout())
    {
        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        home_page_callback();
    }
}

// ====================== 页面绘制函数（已修正字体大小） ======================
void show_oled_param_settings_page(u8g2_t *u8g2)
{
    u8g2_FirstPage(u8g2);
    do
    {
        // 标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        // 【修改1：标题改用14号加粗字体】
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Param Settings")) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, "Param Settings");
        u8g2_SetDrawColor(u8g2, 1);

        // 菜单边框
        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44);
        uint16_t menu_y = 32;
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);

        for (uint8_t i = param_settings_menu.top_index; i < param_settings_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            if (i >= param_settings_menu.item_total) break;
            if (i == param_settings_menu.cur_index)
            {
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, param_settings_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, param_settings_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, param_settings_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, param_settings_menu.items[i].item_name);
            }
            menu_y += MENU_ITEM_H;
        }

        // 底部状态栏
        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        // 【修改2：底部提示小字6x10】
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Rotate Select | Long Press Back")) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, "Rotate Select | Long Press Back");
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void show_oled_comm_settings_page(u8g2_t *u8g2)
{
    u8g2_FirstPage(u8g2);
    do
    {
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Comm Settings")) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, "Comm Settings");
        u8g2_SetDrawColor(u8g2, 1);

        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44);
        uint16_t menu_y = 32;
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);

        for (uint8_t i = comm_settings_menu.top_index; i < comm_settings_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            if (i >= comm_settings_menu.item_total) break;
            if (i == comm_settings_menu.cur_index)
            {
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, comm_settings_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, comm_settings_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, comm_settings_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, comm_settings_menu.items[i].item_name);
            }
            menu_y += MENU_ITEM_H;
        }

        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Rotate Select | Long Press Back")) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, "Rotate Select | Long Press Back");
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void show_oled_system_info_page(u8g2_t *u8g2)
{
    u8g2_FirstPage(u8g2);
    do
    {
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "System Info")) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, "System Info");
        u8g2_SetDrawColor(u8g2, 1);

        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44);
        uint16_t menu_y = 32;
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);

        for (uint8_t i = system_info_menu.top_index; i < system_info_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            if (i >= system_info_menu.item_total) break;
            if (i == system_info_menu.cur_index)
            {
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, system_info_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, system_info_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, system_info_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, system_info_menu.items[i].item_name);
            }
            menu_y += MENU_ITEM_H;
        }

        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Rotate Select | Long Press Back")) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, "Rotate Select | Long Press Back");
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void show_oled_debug_mode_page(u8g2_t *u8g2)
{
    u8g2_FirstPage(u8g2);
    do
    {
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Debug Mode")) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, "Debug Mode");
        u8g2_SetDrawColor(u8g2, 1);

        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44);
        uint16_t menu_y = 32;
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);

        for (uint8_t i = debug_mode_menu.top_index; i < debug_mode_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            if (i >= debug_mode_menu.item_total) break;
            if (i == debug_mode_menu.cur_index)
            {
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, debug_mode_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, debug_mode_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, debug_mode_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, debug_mode_menu.items[i].item_name);
            }
            menu_y += MENU_ITEM_H;
        }

        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Rotate Select | Long Press Back")) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, "Rotate Select | Long Press Back");
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

void show_oled_alarm_record_page(u8g2_t *u8g2)
{
    u8g2_FirstPage(u8g2);
    do
    {
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Alarm Record")) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, "Alarm Record");
        u8g2_SetDrawColor(u8g2, 1);

        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44);
        uint16_t menu_y = 32;
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);

        for (uint8_t i = alarm_record_menu.top_index; i < alarm_record_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            if (i >= alarm_record_menu.item_total) break;
            if (i == alarm_record_menu.cur_index)
            {
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, alarm_record_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, alarm_record_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, alarm_record_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, alarm_record_menu.items[i].item_name);
            }
            menu_y += MENU_ITEM_H;
        }

        u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        u8g2_SetDrawColor(u8g2, 0);
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Rotate Select | Long Press Back")) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, "Rotate Select | Long Press Back");
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

// 字体定义分层
#define TITLE_FONT    u8g2_font_ncenB12_tr   // 顶部标题超大字体
#define VALUE_FONT    u8g2_font_10x20_tf   // 参数数值字体
#define STATUS_FONT   u8g2_font_6x10_tf    // 底部状态栏小字

#define COMM_SETTINGS_ITEM_CNT (sizeof(comm_settings_items)/sizeof(MenuItem))

/**
 * @brief 统一绘制顶部标题 + 底部状态栏
 * @param title 页面标题字符串
 */
static void draw_menu_header_status(const char *title)
{
    u8g2_t *u8g2 = &g_u8g2;

    // ========== 1. 顶部标题黑底栏（完全照搬你的菜单绘制逻辑）==========
    u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
    u8g2_SetFont(u8g2, TITLE_FONT); // 和菜单标题字体一致
    u8g2_SetDrawColor(u8g2, 0); // 黑底白色文字
    uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, title)) / 2;
    u8g2_DrawStr(u8g2, title_x, 16, title);
    u8g2_SetDrawColor(u8g2, 1); // 恢复正常绘制颜色

    // ========== 2. 中间内容边框（和菜单边框逻辑一致）==========
    // 高度 = 总高度 - 标题高度22 - 底部状态栏高度22
    u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44);

    // ====================== 底部状态栏======================
    u8g2_DrawBox(u8g2, 0, OLED_HEIGHT - 18, OLED_WIDTH, 18);
    u8g2_SetFont(u8g2, STATUS_FONT);
    u8g2_SetDrawColor(u8g2, 0); // 黑底白色文字
    const char *status_text = "Enc:+/- Short:SAVE Long:Exit";
    uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, status_text)) / 2;
    u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, status_text);
    u8g2_SetDrawColor(u8g2, 1); // 恢复绘制颜色
}

// ====================== 参数编辑页面（预留超时跳转框架，自行填充编辑逻辑） ======================
// 1. 基准压力 Ref Pressure
void param_ref_press_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    float edit_val = sys_cfg.ref_pressure;
    const float STEP = 0.1f;
    const float MIN_VAL = 0.0f;
    const float MAX_VAL = 30.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        // 绘制顶部标题 + 底部状态栏
        draw_menu_header_status("Ref Pressure(MPa)");

        // 中间数值区域
        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%.2f(MPa) ", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.ref_pressure = edit_val;
                    sync_to_sysdb();
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 2. 最高保护压力 Max Pressure
void param_max_press_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    float edit_val = sys_cfg.max_pressure;
    const float STEP = 0.1f;
    const float MIN_VAL = 0.5f;
    const float MAX_VAL = 40.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Max Pressure(MPa)");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%.2f (MPa)", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.max_pressure = edit_val;
                    sync_to_sysdb();
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 3. 电机最高转速 Motor Max RPM
void param_motor_max_rpm_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    float edit_val = sys_cfg.max_motor_speed;
    const float STEP = 10.0f;
    const float MIN_VAL = 100.0f;
    const float MAX_VAL = 6000.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Motor Max RPM");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%.0f (RPM)", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.max_motor_speed = edit_val;
                    sync_to_sysdb();
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 4. 电机最低转速 Motor Min RPM
void param_motor_min_rpm_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    float edit_val = sys_cfg.min_motor_speed;
    const float STEP = 10.0f;
    const float MIN_VAL = 0.0f;
    const float MAX_VAL = 1000.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Motor Min RPM");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%.0f (RPM)", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.min_motor_speed = edit_val;
                    sync_to_sysdb();
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 5. 油温报警阈值 Temp Alarm Thres
void param_temp_thres_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    float edit_val = sys_cfg.max_oil_temp;
    const float STEP = 1.0f;
    const float MIN_VAL = -50.0f;
    const float MAX_VAL = 120.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Oil Temp Thres(C)");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%.1f (C)", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.max_oil_temp = edit_val;
                    sync_to_sysdb();
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 6. 最低液位报警阈值 Liquid Level Thres
void param_level_thres_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    float edit_val = sys_cfg.min_liquid_level;
    const float STEP = 1.0f;
    const float MIN_VAL = 0.0f;
    const float MAX_VAL = 50.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Liquid Thres(%)");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%.1f (%%)", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.min_liquid_level = edit_val;
                    sync_to_sysdb();
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 7. PID P/I/D 三合一设置页面
void param_pid_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t sel_idx = 0; // 0:Kp 1:Ki 2:Kd
    float kp = sys_cfg.pid_kp;
    float ki = sys_cfg.pid_ki;
    float kd = sys_cfg.pid_kd;
    const float STEP = 0.01f;
    const float PID_MIN = 0.0f;
    const float PID_MAX = 10.0f;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        // PID页面标题
        draw_menu_header_status("PID PARAM SET");

        // 中间三行PID参数
        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        if (sel_idx == 0)
            snprintf(buf, sizeof(buf), ">Kp:%.2f", kp);
        else
            snprintf(buf, sizeof(buf), " Kp:%.2f", kp);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        if (sel_idx == 1)
            snprintf(buf, sizeof(buf), ">Ki:%.2f", ki);
        else
            snprintf(buf, sizeof(buf), " Ki:%.2f", ki);
        u8g2_DrawStr(&g_u8g2, 10, 64, buf);

        if (sel_idx == 2)
            snprintf(buf, sizeof(buf), ">Kd:%.2f", kd);
        else
            snprintf(buf, sizeof(buf), " Kd:%.2f", kd);
        u8g2_DrawStr(&g_u8g2, 10, 80, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    if (sel_idx == 0)
                    {
                        kp += STEP;
                        if (kp > PID_MAX) kp = PID_MAX;
                    }
                    else if (sel_idx == 1)
                    {
                        ki += STEP;
                        if (ki > PID_MAX) ki = PID_MAX;
                    }
                    else
                    {
                        kd += STEP;
                        if (kd > PID_MAX) kd = PID_MAX;
                    }
                    break;
                case ENCODER_CCW:
                    if (sel_idx == 0)
                    {
                        kp -= STEP;
                        if (kp < PID_MIN) kp = PID_MIN;
                    }
                    else if (sel_idx == 1)
                    {
                        ki -= STEP;
                        if (ki < PID_MIN) ki = PID_MIN;
                    }
                    else
                    {
                        kd -= STEP;
                        if (kd < PID_MIN) kd = PID_MIN;
                    }
                    break;
                case ENCODER_PUTH:
                    sel_idx = (sel_idx + 1) % 3;
                    break;
                case ENCODER_PUTH_LONG:
                    sys_cfg.pid_kp = kp;
                    sys_cfg.pid_ki = ki;
                    sys_cfg.pid_kd = kd;
                    sync_to_sysdb();
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        // 清空所有未处理的编码器事件
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;

        // 新增：清空二级参数菜单残留索引（核心修复）
        param_settings_menu.cur_index = 0;
        param_settings_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// ===================== 通讯设置页面=====================
// 1. Modbus从站地址设置
void comm_modbus_addr_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t edit_val = sys_cfg.modbus_addr;
    const uint8_t STEP = 1;
    const uint8_t MIN_VAL = 1;
    const uint8_t MAX_VAL = 247;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Modbus Slave Addr");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "Addr: %d", edit_val);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    edit_val += STEP;
                    if (edit_val > MAX_VAL) edit_val = MAX_VAL;
                    break;
                case ENCODER_CCW:
                    edit_val -= STEP;
                    if (edit_val < MIN_VAL) edit_val = MIN_VAL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.modbus_addr = edit_val;
                    sync_to_sysdb(); // Modbus参数存入系统配置库
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        comm_settings_menu.cur_index = 0;
        comm_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 2. UART波特率设置
void comm_baudrate_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t sel_idx = sys_cfg.modbus_baud_sel;
    const uint8_t MIN_SEL = 1;
    const uint8_t MAX_SEL = 6;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("UART Baudrate");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "Rate: %lu", GetModbusBaudBySel(sel_idx));
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    sel_idx++;
                    if (sel_idx > MAX_SEL) sel_idx = MAX_SEL;
                    break;
                case ENCODER_CCW:
                    sel_idx--;
                    if (sel_idx < MIN_SEL) sel_idx = MIN_SEL;
                    break;
                case ENCODER_PUTH:
                    sys_cfg.modbus_baud_sel = sel_idx;
                    sync_to_sysdb(); // Modbus波特率存入系统配置库
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        comm_settings_menu.cur_index = 0;
        comm_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 3. UART参数占位页面（无保存逻辑）
void comm_uart_param_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("UART Parity/Data");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        u8g2_DrawStr(&g_u8g2, 10, 48, "Unavailable Now");

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        comm_settings_menu.cur_index = 0;
        comm_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 4. ETH IP地址编辑页面
void comm_ip_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t temp_ip[4];
    memcpy(temp_ip, g_net_cfg.ip, 4);
    uint8_t seg_idx = 0; // 当前编辑段 0~3
    const uint8_t STEP = 1;
    const uint8_t SEG_MIN = 0;
    const uint8_t SEG_MAX = 255;
    char buf[40];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("ETH IP Address");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d", temp_ip[0], temp_ip[1], temp_ip[2], temp_ip[3]);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    temp_ip[seg_idx] += STEP;
                    if (temp_ip[seg_idx] > SEG_MAX) temp_ip[seg_idx] = SEG_MAX;
                    break;
                case ENCODER_CCW:
                    temp_ip[seg_idx] -= STEP;
                    if (temp_ip[seg_idx] < SEG_MIN) temp_ip[seg_idx] = SEG_MIN;
                    break;
                case ENCODER_PUTH:
                    seg_idx = (seg_idx + 1) % 4; // 短按切换IP段，不保存
                    break;
                case ENCODER_PUTH_LONG:
                    memcpy(g_net_cfg.ip, temp_ip, 4);
                    net_config_save(); // 网络参数独立保存接口
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        comm_settings_menu.cur_index = 0;
        comm_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 5. ETH子网掩码编辑页面
void comm_netmask_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t temp_mask[4];
    memcpy(temp_mask, g_net_cfg.netmask, 4);
    uint8_t seg_idx = 0;
    const uint8_t STEP = 1;
    const uint8_t SEG_MIN = 0;
    const uint8_t SEG_MAX = 255;
    char buf[40];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("ETH Netmask");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d", temp_mask[0], temp_mask[1], temp_mask[2], temp_mask[3]);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    temp_mask[seg_idx] += STEP;
                    if (temp_mask[seg_idx] > SEG_MAX) temp_mask[seg_idx] = SEG_MAX;
                    break;
                case ENCODER_CCW:
                    temp_mask[seg_idx] -= STEP;
                    if (temp_mask[seg_idx] < SEG_MIN) temp_mask[seg_idx] = SEG_MIN;
                    break;
                case ENCODER_PUTH:
                    seg_idx = (seg_idx + 1) % 4;
                    break;
                case ENCODER_PUTH_LONG:
                    memcpy(g_net_cfg.netmask, temp_mask, 4);
                    net_config_save(); // 网络参数独立保存接口
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        comm_settings_menu.cur_index = 0;
        comm_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 6. ETH网关地址编辑页面
void comm_gateway_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t temp_gw[4];
    memcpy(temp_gw, g_net_cfg.gateway, 4);
    uint8_t seg_idx = 0;
    const uint8_t STEP = 1;
    const uint8_t SEG_MIN = 0;
    const uint8_t SEG_MAX = 255;
    char buf[40];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("ETH Gateway");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d", temp_gw[0], temp_gw[1], temp_gw[2], temp_gw[3]);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    temp_gw[seg_idx] += STEP;
                    if (temp_gw[seg_idx] > SEG_MAX) temp_gw[seg_idx] = SEG_MAX;
                    break;
                case ENCODER_CCW:
                    temp_gw[seg_idx] -= STEP;
                    if (temp_gw[seg_idx] < SEG_MIN) temp_gw[seg_idx] = SEG_MIN;
                    break;
                case ENCODER_PUTH:
                    seg_idx = (seg_idx + 1) % 4;
                    break;
                case ENCODER_PUTH_LONG:
                    memcpy(g_net_cfg.gateway, temp_gw, 4);
                    net_config_save(); // 网络参数独立保存接口
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        comm_settings_menu.cur_index = 0;
        comm_settings_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// ===================== 系统信息页面=====================
// 1. 固件版本信息页面
void info_fw_ver_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Firmware Version");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "Ver: %s", DEVICE_FW_VER);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            // 仅支持长按退出，旋转/短按无任何编辑功能
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    // 超时仅清理当前系统信息菜单索引
    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 2. 硬件版本信息页面
void info_hw_ver_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Hardware Version");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "Ver: %s", DEVICE_HW_VER);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 3. 运行状态信息页面
void info_runtime_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[40];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;

        // 每一帧刷新系统运行时间，动态更新
        uint32_t tick = xTaskGetTickCount();
        uint32_t sec = tick / configTICK_RATE_HZ;
        uint32_t h = sec / 3600;
        uint32_t m = (sec % 3600) / 60;
        uint32_t s = sec % 60;

        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Runtime Stat");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "%02"PRIu32":%02"PRIu32":%02"PRIu32, h, m, s);
        u8g2_DrawStr(&g_u8g2, 10, 48, buf);

        u8g2_SendBuffer(&g_u8g2);

        // 等待按键，100ms刷新一次画面
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 4. 传感器原始数据信息页面
void info_sensor_raw_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[48];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Sensor Raw ADC");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "CH0:%4d  CH1:%4d", adc_raw_backup[0], adc_raw_backup[1]);
        u8g2_DrawStr(&g_u8g2, 8, 42, buf);
        snprintf(buf, sizeof(buf), "CH2:%4d  CH3:%4d", adc_raw_backup[2], adc_raw_backup[3]);
        u8g2_DrawStr(&g_u8g2, 8, 62, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// ===================== 调试模式页面=====================
// 1. 手动控制
void debug_manual_ctrl_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint8_t sel_idx = 0; // 0:电机转速  1:阀门开关
    float edit_speed = 0.0f;
    const float SPD_STEP = 5.0f;
    SYS_CONFIG_T cfg_tmp = sys_cfg;
    _SYS_STATUS sys_tmp;
    SysStatus_ReadSnapshot(&sys_tmp);
    char buf[32];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Manual Motor/Valve");

        // 状态提示：仅设置模式可用
        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        if(sys_tmp.sys_start != SYS_STATE_SETTING)
        {
            u8g2_DrawStr(&g_u8g2, 10, 48, "Need Enter Setting Mode!");
            u8g2_DrawStr(&g_u8g2, 10, 68, "Long Press Back");
        }
        else
        {
            // 读取当前手动转速
            edit_speed = sys_tmp.manual_set_speed;
            // 选项1：电机转速
            if(sel_idx == 0)
            {
                u8g2_DrawStr(&g_u8g2, 0, 30, ">Motor Speed(rpm)");
                u8g2_SetFont(&g_u8g2, VALUE_FONT);
                snprintf(buf, sizeof(buf), "%.1f", edit_speed);
                u8g2_DrawStr(&g_u8g2, 80, 30, buf);
            }
            else
            {
                u8g2_DrawStr(&g_u8g2, 0, 30, " Motor Speed(rpm)");
            }
            // 选项2：泄压阀
            if(sel_idx == 1)
            {
                u8g2_DrawStr(&g_u8g2, 0, 50, ">Valve State");
                u8g2_SetFont(&g_u8g2, VALUE_FONT);
                snprintf(buf, sizeof(buf), sys_tmp.valve_state ? "OPEN" : "CLOSE");
                u8g2_DrawStr(&g_u8g2, 80, 50, buf);
            }
            else
            {
                u8g2_DrawStr(&g_u8g2, 0, 50, " Valve State");
            }
            u8g2_SetFont(&g_u8g2, STATUS_FONT);
            u8g2_DrawStr(&g_u8g2, 0, 72, "Enc:Switch | Short:Edit/Save | Long:Back");
        }

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    if(sys_tmp.sys_start != SYS_STATE_SETTING) break;
                    if(sel_idx == 0)
                    {
                        edit_speed += SPD_STEP;
                        if(edit_speed > cfg_tmp.max_motor_speed) edit_speed = cfg_tmp.max_motor_speed;
                        // 更新手动转速快照
                        _SYS_STATUS write_tmp;
                        SysStatus_ReadSnapshot(&write_tmp);
                        write_tmp.manual_set_speed = edit_speed;
                        SysStatus_WriteSnapshot(&write_tmp);
                    }
                    else
                    {
                        sel_idx = 0;
                    }
                    break;
                case ENCODER_CCW:
                    if(sys_tmp.sys_start != SYS_STATE_SETTING) break;
                    if(sel_idx == 0)
                    {
                        edit_speed -= SPD_STEP;
                        if(edit_speed < 0.0f) edit_speed = 0.0f;
                        _SYS_STATUS write_tmp;
                        SysStatus_ReadSnapshot(&write_tmp);
                        write_tmp.manual_set_speed = edit_speed;
                        SysStatus_WriteSnapshot(&write_tmp);
                    }
                    else
                    {
                        sel_idx = 1;
                    }
                    break;
                case ENCODER_PUTH:
                    if(sys_tmp.sys_start != SYS_STATE_SETTING) break;
                    // 阀门短按切换状态
                    if(sel_idx == 1)
                    {
                        _SYS_STATUS write_tmp;
                        SysStatus_ReadSnapshot(&write_tmp);
                        write_tmp.valve_state = !write_tmp.valve_state;
                        SysStatus_WriteSnapshot(&write_tmp);
                    }
                    break;
                case ENCODER_PUTH_LONG:
                    exit_page = 1;
                    break;
                default: break;
            }
        }
        // 刷新快照实时显示
        SysStatus_ReadSnapshot(&sys_tmp);
    }

    // 超时/退出统一处理逻辑（和模板完全一致）
    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        debug_mode_menu.cur_index = 0;
        debug_mode_menu.top_index = 0;
        
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 2. PID 在线整定
void debug_pid_tune_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[48];

    while (!exit_page)
    {
        // 空闲超时自动退出页面
        if (menu_is_idle_timeout()) break;

        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        // 统一绘制顶部标题栏（替代手动设置标题字体）
        draw_menu_header_status("PID Auto Tune");

        // 内容小字
        u8g2_SetFont(&g_u8g2, STATUS_FONT);
        u8g2_DrawStr(&g_u8g2, 8, 48, "Function Not Implemented");
        u8g2_DrawStr(&g_u8g2, 8, 68, "Long Press Return");

        u8g2_SendBuffer(&g_u8g2);

        // 标准按键队列读取
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            // 长按退出当前页面
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    // 超时/长按退出后统一处理
    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        // 清空残留按键
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);
        // 重置菜单游标
        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 3. IO 状态监视
void debug_io_monitor_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[48];

    while (!exit_page)
    {
        // 空闲超时自动退出页面
        if (menu_is_idle_timeout()) break;

        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        // 统一绘制顶部标题栏（替代手动设置标题字体）
        draw_menu_header_status("IO Monitor");

        // 内容小字
        u8g2_SetFont(&g_u8g2, STATUS_FONT);
        u8g2_DrawStr(&g_u8g2, 8, 48, "Function Not Implemented");
        u8g2_DrawStr(&g_u8g2, 8, 68, "Long Press Return");

        u8g2_SendBuffer(&g_u8g2);

        // 标准按键队列读取
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            // 长按退出当前页面
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    // 超时/长按退出后统一处理
    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        // 清空残留按键
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);
        // 重置菜单游标
        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 4. Modbus 通讯测试
void debug_modbus_test_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[48];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;

        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Modbus Test");

        u8g2_SetFont(&g_u8g2, STATUS_FONT);
        u8g2_DrawStr(&g_u8g2, 8, 48, "Function Not Implemented");
        u8g2_DrawStr(&g_u8g2, 8, 68, "Long Press Return");

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);
        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 5. ADC 原始值监控
void debug_adc_watch_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    char buf[48];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("ADC Raw Monitor");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        snprintf(buf, sizeof(buf), "CH0:%4d  CH1:%4d", adc_raw_backup[0], adc_raw_backup[1]);
        u8g2_DrawStr(&g_u8g2, 8, 42, buf);
        snprintf(buf, sizeof(buf), "CH2:%4d  CH3:%4d", adc_raw_backup[2], adc_raw_backup[3]);
        u8g2_DrawStr(&g_u8g2, 8, 62, buf);

        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            if (encoder_state == ENCODER_PUTH_LONG)
            {
                exit_page = 1;
            }
        }
    }
    // 超时自动回首页逻辑
    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);
        system_info_menu.cur_index = 0;
        system_info_menu.top_index = 0;
        menu_reset_idle_timer();
        home_page_callback();
    }
}

// ===================== 报警记录页面=====================
// 1. 历史报警记录浏览页面
void alarm_history_list_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    uint16_t disp_top_idx = 0;  // 显示第一条记录索引
    const uint16_t DISP_LINE_CNT = 4; // 一屏最多显示4条报警
    char buf[64];

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        // 绘制标题+底部状态栏
        draw_menu_header_status("History Alarm List");

        u8g2_SetFont(&g_u8g2, STATUS_FONT);
        // 循环渲染当前屏4条报警
        for(uint8_t line = 0; line < DISP_LINE_CNT; line++)
        {
            uint16_t rec_idx = disp_top_idx + line;
            if(rec_idx >= ALARM_RECORD_MAX_CNT) break;

            // 读取一条报警记录（假设全局存储数组 AlarmRecord g_alarm_records[ALARM_RECORD_MAX_CNT];）
            AlarmRecord *rec = &g_alarm_records[rec_idx];
            snprintf(buf, sizeof(buf),
                     "%02d:%02d:%02d Bit:0x%02X",
                     rec->hour, rec->min, rec->sec, rec->fault_bit);
            u8g2_DrawStr(&g_u8g2, 6, 34 + line * 16, buf);
        }

        u8g2_SendBuffer(&g_u8g2);

        // 编码器事件处理
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW: // 向下翻页，索引+1
                    if(disp_top_idx + DISP_LINE_CNT < ALARM_RECORD_MAX_CNT)
                        disp_top_idx++;
                    break;
                case ENCODER_CCW: // 向上翻页，索引-1
                    if(disp_top_idx > 0)
                        disp_top_idx--;
                    break;
                case ENCODER_PUTH_LONG: // 长按退出
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    // 空闲超时自动回首页逻辑（和所有菜单模板统一）
    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        alarm_record_menu.cur_index = 0;
        alarm_record_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
}

// 2. 清除全部历史报警记录确认页
void alarm_clear_all_callback(void)
{
    menu_reset_idle_timer();
    EncoderState encoder_state = ENCODER_NONE;
    uint8_t exit_page = 0;
    // 当前选中项：0=清空报警记录，1=故障复位
    uint8_t sel_idx = 0;
    // 选项文本数组
    const char *menu_items[] = {
        "Clear Alarm Record",
        "Fault Reset Request"
    };
    const uint8_t item_cnt = sizeof(menu_items)/sizeof(menu_items[0]);

    while (!exit_page)
    {
        if (menu_is_idle_timeout()) break;
        u8g2_ClearBuffer(&g_u8g2);
        u8g2_SetDrawColor(&g_u8g2, 1);

        draw_menu_header_status("Alarm Operation");

        u8g2_SetFont(&g_u8g2, VALUE_FONT);
        // 绘制两个选项，带光标区分选中项
        for(uint8_t i = 0; i < item_cnt; i++)
        {
            if(i == sel_idx)
            {
                u8g2_DrawStr(&g_u8g2, 4, 44 + i*20, ">");
            }
            u8g2_DrawStr(&g_u8g2, 16, 44 + i*20, menu_items[i]);
        }
        u8g2_SendBuffer(&g_u8g2);

        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {
            menu_reset_idle_timer();
            switch (encoder_state)
            {
                case ENCODER_CW:
                    // 旋转加，切换下一项
                    sel_idx = (sel_idx + 1) % item_cnt;
                    break;
                case ENCODER_CCW:
                    // 旋转减，切换上一项
                    sel_idx = (sel_idx + item_cnt - 1) % item_cnt;
                    break;
                case ENCODER_PUTH:
                {
                    // 短按执行选中功能
                    if(sel_idx == 0)
                    {
                        // 选项1：仅清空报警记录
                        memset(g_alarm_records, 0, sizeof(g_alarm_records));
                        g_alarm_record_cnt = 0;
                    }
                    else if(sel_idx == 1)
                    {
                        // 选项2：故障复位请求写入快照
                        _SYS_STATUS write_tmp = {0};
                        write_tmp.fault_reset_req = 1;
                        SysStatus_WriteSnapshot(&write_tmp);
                    }
                    exit_page = 1;
                    break;
                }
                case ENCODER_PUTH_LONG:
                    // 长按取消，不执行任何操作
                    exit_page = 1;
                    break;
                default: break;
            }
        }
    }

    if (menu_is_idle_timeout())
    {
        EncoderState tmp_state;
        // 清空残留按键
        while (xQueueReceive(QUEUE_KEY, &tmp_state, 0) == pdPASS);

        // 重置报警菜单索引
        g_menu.cur_index = 0;
        g_menu.top_index = 0;
        alarm_record_menu.cur_index = 0;
        alarm_record_menu.top_index = 0;

        menu_reset_idle_timer();
        home_page_callback();
    }
    else
    {
        // 操作完成/取消，返回报警记录菜单
        alarm_history_list_callback();
    }
}


/**
 * @brief 显示温湿度页面
 */
void show_temperature_humidity_page(u8g2_t *u8g2)
{
    _SYS_STATUS sys_tmp;
    SYS_CONFIG_T cfg_tmp;

    // 【重点】渲染前一次性读取系统快照，整页画面使用同一组数据，防止画面撕裂
    SysStatus_ReadSnapshot(&sys_tmp);
    cfg_tmp = sys_cfg;

    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "BORCH Oil Pump")) / 2, 18, "BORCH Oil Pump");

        // M：电机故障指示灯
        if ((sys_tmp.motor_status.err_code != 0) 
            || (sys_tmp.motor_status.status_word & STATUS_BIT_FAULT)
            || (sys_tmp.motor_status.comm_lost != 0))
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 12, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 5, 39, "M");
        }

        // S：传感器故障指示灯（修正枚举判断错误）
        //if (1)
        if (sys_tmp.sensor_status != SENSOR_STATUS_READY)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 32, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 27, 39, "S");
        }

        // L：液位过低告警指示灯
        //if (1)
        if(sys_tmp.liquid_level_pct <= cfg_tmp.min_liquid_level)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 52, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 47, 39, "L");
        }

        // P：超压指示灯（修正枚举判断错误）
        float severe_overpress_thr = cfg_tmp.max_pressure + cfg_tmp.overpress_margin;
        //if (1)
        if(sys_tmp.adc_pressure >= severe_overpress_thr)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 72, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 67, 39, "P");
        }

        // T：油温过高告警指示灯
        //if (1)
        if(sys_tmp.adc_oil_temp >= cfg_tmp.max_oil_temp)
        {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawDisc(u8g2, 92, 32, 9, U8G2_DRAW_ALL);
            u8g2_SetFont(u8g2, u8g2_font_ncenB12_tr);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 87, 39, "T");
        }



        // 下面全部保持 DrawColor=1
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_SetFont(u8g2, u8g2_font_7x13_tf);
        char buf[32] = {0};

        // ==========同一行右侧显示设备ID ==========
        snprintf(buf, sizeof(buf), "ID:%s", &g_device_sn[6]);
        uint16_t sn_width = u8g2_GetStrWidth(u8g2, buf);
        uint16_t sn_x = OLED_WIDTH - sn_width - 5;
        u8g2_DrawStr(u8g2, sn_x, 39, buf);
        memset(buf, 0, sizeof(buf));

        //下面统一使用9x15_tf字体
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        // ==========当前压力 + 基准压力 ==========
        float2str(sys_tmp.adc_pressure, 2, buf, 32);
        u8g2_printf(u8g2, 10, 60, "Pr: %sMPa", buf);
        memset(buf, 0, sizeof(buf));

        float2str(cfg_tmp.ref_pressure, 2, buf, 32);
        u8g2_printf(u8g2, 135, 60, "Ref: %sMPa", buf);
        memset(buf, 0, sizeof(buf));

        // ==========电机转速 + 油液温度 ==========
        u8g2_printf(u8g2, 10, 80, "RPM: %lurpm", sys_tmp.motor_status.actual_speed);
        float2str(sys_tmp.adc_oil_temp, 2, buf, 32);
        u8g2_printf(u8g2, 135, 80, "OT: %sC", buf);
        memset(buf, 0, sizeof(buf));

        // ========== 液位 + 阀门状态 ==========
        float2str(sys_tmp.liquid_level_pct, 1, buf, sizeof(buf));
        u8g2_printf(u8g2, 10, 100, "Level: %s%%", buf);
        memset(buf, 0, sizeof(buf));
        if(sys_tmp.valve_state == 1)
        {
            u8g2_DrawStr(u8g2, 135, 100, "Valve: OPEN");
        }
        else
        {
            u8g2_DrawStr(u8g2, 135, 100, "Valve: CLS");
        }

        // // ==========温度 + 湿度 ==========
        // u8g2_SetDrawColor(u8g2, 1);
        // u8g2_SetFont(u8g2, u8g2_font_9x15_tf);
        // SHT40_Read_Temperature_Humidity(&Temperature, &Humidity);
        // char buf[32] = {0};
        // float2str(Temperature, 2, buf, 32);
        // u8g2_printf(u8g2, 20, 80, "T: %sC", buf);
        // memset(buf, 0, sizeof(buf));
        // float2str(Humidity, 2, buf, 32);
        // u8g2_printf(u8g2, 148, 80, "H: %s%s", buf, "%");

        // ==========时间显示==========
        // 电机无故障 并且 传感器就绪，才显示时间
        if( (sys_tmp.motor_status.err_code == 0) && !(sys_tmp.motor_status.status_word & STATUS_BIT_FAULT)
            && (sys_tmp.sensor_status == SENSOR_STATUS_READY) && (sys_tmp.adc_pressure < severe_overpress_thr) && (sys_tmp.adc_oil_temp < cfg_tmp.max_oil_temp))
        {
            u8g2_SetFont(u8g2, u8g2_font_7x13_tf);
            refresh_rtc_time();
            u8g2_printf(u8g2, 10, 39, "20%02d.%02d.%02d %02d:%02d:%02d",
                        RtcData.Year, RtcData.Month, RtcData.Date,
                        RtcTime.Hours, RtcTime.Minutes, RtcTime.Seconds);
        }

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
void home_page_callback(void)
{
    menu_reset_idle_timer();
    // 显示温湿度页面
    extern u8g2_t g_u8g2;
    show_temperature_humidity_page(&g_u8g2);

    // 等待长按返回
    EncoderState encoder_state;
    uint32_t start_time = xTaskGetTickCount();
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        vTaskDelay(1);
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











void system_settings_callback(void)
{

    menu_init(&system_settings_menu, system_settings_menu_items, sizeof(system_settings_menu_items) / sizeof(MenuItem));

    EncoderState encoder_state;
    // 显示菜单页面
    show_oled_system_settings_menu_page(&g_u8g2);
    uint8_t in_system_settings = 1;

    while (in_system_settings)
    {
        // 检查是否有编码器事件
        if (xQueueReceive(QUEUE_KEY, &encoder_state, portMAX_DELAY))
        {
            // 处理编码器事件
            switch (encoder_state)
            {
            case ENCODER_CW: // 顺时针旋转 - 向下导航
                system_settings_menu_handle_event(MENU_EVENT_DOWN, &in_system_settings);
                show_oled_system_settings_menu_page(&g_u8g2);
                break;
            case ENCODER_CCW: // 逆时针旋转 - 向上导航
                system_settings_menu_handle_event(MENU_EVENT_UP, &in_system_settings);
                show_oled_system_settings_menu_page(&g_u8g2);
                break;
            case ENCODER_PUTH: // 短按按钮 - 确认选择
                system_settings_menu_handle_event(MENU_EVENT_ENTER, &in_system_settings);
                break;
            case ENCODER_PUTH_LONG: // 长按按钮 - 返回操作
                system_settings_menu_handle_event(MENU_EVENT_BACK, &in_system_settings);
                in_system_settings = 0;
                show_oled_system_settings_menu_page(&g_u8g2);
                break;
            default:
                break;
            }
        }
    }
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

void p_model_init_callback(void)
{
    // 停止系统运行，进入设置模式
    main_sys_status.sys_start = SYS_STATE_SETTING;
    uint16_t angle = 1;
    // P模型初始化页面
    extern u8g2_t g_u8g2;
    show_p_model_init_page(&g_u8g2, angle, "Init ...");
    // 读取当前P电机角度
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
                main_sys_status.sys_start = SYS_STATE_SETTING;
                break;
            case ENCODER_PUTH:


                break;
            case ENCODER_CW:
                show_p_model_init_page(&g_u8g2, angle, "Ready!");
                break;
            case ENCODER_CCW:
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

void q_model_init_callback(void)
{
    // 停止系统运行，进入设置模式
    main_sys_status.sys_start = SYS_STATE_SETTING;

    // Q模型初始化页面
    extern u8g2_t g_u8g2;


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
                main_sys_status.sys_start = SYS_STATE_SETTING;
                break;
            case ENCODER_PUTH:

                break;
            case ENCODER_CW:

                break;
            case ENCODER_CCW:
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

void test_running_callback(void)
{

}

void about_device_callback(void)
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
        // 获取并显示芯片唯一序列号
        STM32_UIDTypeDef uid;
        STM32_GetUID(&uid);
        u8g2_printf(&g_u8g2, 10, 90, "%08X-%08X-%08X", uid.uid2, uid.uid1, uid.uid0);

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

void reboot_callback(void)
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

    // 延时后重启
    vTaskDelay(pdMS_TO_TICKS(2000));
    // 执行重启操作
    reboot_system();
}

/**
 * @brief 单元测试运行页面
 */
void unit_test_running_page(u8g2_t *u8g2, uint16_t p_angle, uint16_t q_angle, char sen_top, char sen_bottom, uint16_t running_tag)
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

}

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
void show_oled_main_menu_page(u8g2_t *u8g2)
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

/**
 * @brief 显示系统设置菜单
 * @param u8g2
 */
void show_oled_system_settings_menu_page(u8g2_t *u8g2)
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
        uint16_t title_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "System Settings")) / 2;
        u8g2_DrawStr(u8g2, title_x, 16, "System Settings"); // 标题垂直居中（24像素高度，y=16最佳）
        u8g2_SetDrawColor(u8g2, 1);                         // 恢复默认绘制颜色

        // -------------------------- 第二段：菜单列表（核心，清晰规整） --------------------------
        // 1. 菜单列表背景（浅淡分隔，与标题栏/状态栏区分）
        u8g2_DrawFrame(u8g2, 0, 24, OLED_WIDTH, OLED_HEIGHT - 44); // 空心框，形成列表区域

        // 2. 绘制可见菜单项（循环绘制，支持滚动）
        uint16_t menu_y = 32;                  // 菜单列表起始y坐标（标题栏下方8像素，优雅间距）
        u8g2_SetFont(u8g2, u8g2_font_9x15_tf); // 中等字体，清晰易读，适合菜单

        for (uint8_t i = system_settings_menu.top_index; i < system_settings_menu.top_index + MENU_VISIBLE_MAX; i++)
        {
            // 超出菜单总数，停止绘制
            if (i >= system_settings_menu.item_total)
                break;

            // 选中项：突出显示（实心背景+加粗文字，优雅醒目）
            if (i == system_settings_menu.cur_index)
            {
                // 选中项背景（占据整行，高度与菜单项一致，无违和感）
                u8g2_DrawBox(u8g2, 2, menu_y - 6, OLED_WIDTH - 4, MENU_ITEM_H);

                // 选中项文字（反色显示，与背景形成对比）
                u8g2_SetDrawColor(u8g2, 0);
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, system_settings_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, system_settings_menu.items[i].item_name);
                u8g2_SetDrawColor(u8g2, 1);
            }
            // 未选中项：正常显示（透明背景，清晰整洁）
            else
            {
                uint16_t item_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, system_settings_menu.items[i].item_name)) / 2;
                u8g2_DrawStr(u8g2, item_x, menu_y + 8, system_settings_menu.items[i].item_name);
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
        uint16_t status_x = (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "System Settings")) / 2;
        u8g2_DrawStr(u8g2, status_x, OLED_HEIGHT - 6, "System Settings");
        u8g2_SetDrawColor(u8g2, 1);

    } while (u8g2_NextPage(u8g2));
}

/**
 * @brief 串口设置的回调函数
 *
 */
void serial_port_settings_callback(void)
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
 * @brief P模型速度设置的回调函数
 */
void p_model_speed_settings_callback(void)
{
}

/**
 * @brief Q模型速度设置的回调函数
 */
void q_model_speed_settings_callback(void)
{
}

/**
 * @brief 恢复出厂设置
 */

void restore_factory_settings_callback(void)
{
    // 停止系统运行，进入设置模式
    main_sys_status.sys_start = SYS_STATE_SETTING;

    // Q模型初始化页面
    extern u8g2_t g_u8g2;


    // 进度条位置
    float progress = 0;


    // 等待长按返回
    EncoderState encoder_state;
    uint8_t long_pressed = 0;

    while (!long_pressed)
    {
        show_restore_factory_settings_page(&g_u8g2, progress);
        if (xQueueReceive(QUEUE_KEY, &encoder_state, pdMS_TO_TICKS(100)) == pdPASS)
        {

            switch (encoder_state)
            {
            case ENCODER_PUTH_LONG:
                long_pressed = 1;
                // 返回前设置系统运行模式为SYS_STATE_STOP
                main_sys_status.sys_start  = SYS_STATE_STOP;
                break;
            case ENCODER_PUTH:
                 // 判断进度条是不是在1，如果没有满，不做任何操作，如果满了，进行数据库反初始化并重启
                if(progress == 1)
                {
                    // 反初始化数据库
                    deinit_sysdb();
                    // 重启系统
                    reboot_system();
                }
            
                break;
            case ENCODER_CW:
                if(progress < 1)
                {
                    progress += 0.1f;
                    if(progress > 1)
                    {
                        progress = 1;
                    }
                }
                break;
            case ENCODER_CCW:
              if(progress > 0)
                {
                    progress -= 0.1f;
                    if(progress < 0)
                    {
                        progress = 0;
                    }
                }
                break;

            default:
                break;
            }
        }
    }
}

/**
 * @brief 恢复出厂设置的显示函数
 */
void show_restore_factory_settings_page(u8g2_t *u8g2 , float progress)
{
    u8g2_FirstPage(u8g2);
    do
    {
        // 绘制标题栏
        u8g2_DrawBox(u8g2, 0, 0, OLED_WIDTH, 22);
        u8g2_SetFont(u8g2, u8g2_font_ncenB14_tr);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "RESTORE FACTORY")) / 2, 16, "RESTORE FACTORY");
        u8g2_SetDrawColor(u8g2, 1);

        u8g2_SetFont(u8g2, u8g2_font_9x15_tf); // 中等字体，清晰易读，适合菜单                                                    // 设置字体
        u8g2_DrawStr(u8g2, (OLED_WIDTH - u8g2_GetStrWidth(u8g2, "Confirm factory reset")) / 2, 60, "Confirm factory reset"); // 绘制设备名称

        u8g2_DrawFrame(u8g2, 10, 70, OLED_WIDTH - 20, 20); // 绘制边框

        u8g2_DrawBox(u8g2, 10, 70, (OLED_WIDTH - 20) * progress, 20); // 绘制进度条

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
 *  @brief 系统设置次级菜单事件处理函数
 *  @param event 事件类型
 *  @param menu  菜单控制标志的指针
 */

void system_settings_menu_handle_event(MenuEventType event, uint8_t *menu)
{
    switch (event)
    {
    case MENU_EVENT_UP:
        if (system_settings_menu.cur_index > 0)
        {
            system_settings_menu.cur_index--;
            // 如果当前项移出可见区域，调整top_index
            if (system_settings_menu.cur_index < system_settings_menu.top_index)
            {
                system_settings_menu.top_index--;
            }
        }
        break;

    case MENU_EVENT_DOWN:
        if (system_settings_menu.cur_index < system_settings_menu.item_total - 1)
        {
            system_settings_menu.cur_index++;
            // 如果当前项移出可见区域，调整top_index
            if (system_settings_menu.cur_index >= system_settings_menu.top_index + MENU_VISIBLE_MAX)
            {
                system_settings_menu.top_index++;
            }
        }
        break;

    case MENU_EVENT_ENTER:
        // 执行当前选中项的回调函数
        if (system_settings_menu.items[system_settings_menu.cur_index].callback != NULL)
        {
            system_settings_menu.items[system_settings_menu.cur_index].callback();
        }
        else
        {
            //  没有回调函数默认为返回
            *menu = 0; // 这里可以设置为返回上一级菜单的标志
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
