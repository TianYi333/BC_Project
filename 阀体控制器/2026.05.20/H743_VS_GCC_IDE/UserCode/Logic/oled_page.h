/**
 * @file oled_page.h
 * @brief OLED页面管理头文件
 * @author 梁伟
 * @date 2026-04-20
 */

#ifndef OLED_PAGE_H
#define OLED_PAGE_H

#include "u8g2.h"

// -------------------------- OLED 配置参数 --------------------------
#define OLED_WIDTH  256 // 屏幕宽度
#define OLED_HEIGHT 128 // 屏幕高度

// -------------------------- 开机画面配置 --------------------------
#define DEVICE_LOGO  "BORCH  LUBE" // 设备名称

// -------------------------- 菜单配置 --------------------------
#define MENU_TITLE    "Main Menu"      // 菜单标题
#define MENU_STATUS   "Turn to navigate, enter to select"  // 菜单状态提示
#define MENU_ITEM_H   20            // 菜单项高度
#define MENU_VISIBLE_MAX 4         // 最大可见菜单项数量



// 菜单事件类型
typedef enum {
    MENU_EVENT_UP,       // 向上导航
    MENU_EVENT_DOWN,     // 向下导航
    MENU_EVENT_ENTER,    // 确认选择
    MENU_EVENT_BACK      // 返回上一级
} MenuEventType;

// 菜单项结构体
typedef struct {
    char *item_name;     // 菜单项名称
    void (*callback)(void); // 菜单项回调函数
} MenuItem;

// 菜单结构体
typedef struct {
    MenuItem *items;     // 菜单项数组
    uint8_t item_total;  // 菜单项总数
    uint8_t cur_index;   // 当前选中项索引
    uint8_t top_index;   // 可见区域第一个项索引
} Menu;




// 全局菜单实例
extern Menu g_menu;

/**
 * @brief 显示开机画面
 * @param u8g2 
 */
void show_oled_boot_page(u8g2_t *u8g2);

/**
 * @brief 显示菜单页面
 * @param u8g2 
 */
void show_oled_menu_page(u8g2_t *u8g2);
/**
 * @brief 单元测试运行页面
 */
void unit_test_running_page(u8g2_t *u8g2, uint16_t p_angle, uint16_t q_angle, char sen_top, char sen_bottom,uint16_t running_tag );

/**
 * @brief 处理菜单事件
 * @param event 事件类型
 */
void menu_handle_event(MenuEventType event);

/**
 * @brief 初始化菜单
 * @param menu 菜单指针
 * @param items 菜单项数组
 * @param item_total 菜单项总数
 */
void menu_init(Menu *menu, MenuItem *items, uint8_t item_total);


void menu_item_callback1(void);
void menu_item_callback2(void);
void menu_item_callback3(void);
void menu_item_callback4(void);
void menu_item_callback5(void);
void menu_item_callback6(void);
void menu_item_callback7(void);


// 定义菜单项
extern MenuItem g_menu_items[8]; 

#endif /* OLED_PAGE_H */