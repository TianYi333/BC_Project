/**
 * @file oled_lang.h
 * @brief OLED 中英文双语切换：语言标志、双语选择宏与对外接口
 * @date 2026-11-19
 *
 * 设计要点：
 * 1. cn_font_12/16/24 已含完整 ASCII(0x20~0x7E)，中英文共用同一套字体，
 *    绘制仍走 u8g2_DrawUTF8()，无需按语言切换 SetFont。
 * 2. 语言状态独立存 FlashDB key "oled_lang"（字符串 "0"/"1"），
 *    不进 main_config，因此不影响 Modbus 寄存器映射。
 * 3. 文案在调用点用 OLANG("中文", "English") 就近提供，不集中维护字符串表，
 *    便于逐条核对与后续增删。
 */

#ifndef OLED_LANG_H
#define OLED_LANG_H

#include <stdint.h>

typedef enum
{
    OLED_LANG_CN = 0,   // 中文（默认）
    OLED_LANG_EN = 1    // 英文
} OledLangType;

extern volatile uint8_t g_oled_lang;

/**
 * @brief 就近双语选择宏：中文模式取 cn，英文模式取 en
 * @note 两个分支须为同类型（一般均为字符串字面量），否则条件表达式类型不一致
 */
#define OLANG(cn, en) ((g_oled_lang == OLED_LANG_CN) ? (cn) : (en))

void oled_lang_init(void);   // 上电读取 FlashDB 语言设置，并刷新菜单项名称
void oled_lang_toggle(void); // 切换中/英、写 FlashDB，并刷新菜单项名称

#endif /* OLED_LANG_H */
