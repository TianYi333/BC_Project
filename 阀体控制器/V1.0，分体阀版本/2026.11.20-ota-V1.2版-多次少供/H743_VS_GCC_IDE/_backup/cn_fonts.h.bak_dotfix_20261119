/*
 * cn_fonts.h - U8G2 精简中文点阵字库声明（自动生成，请勿手工编辑）
 */

#ifndef CN_FONTS_H
#define CN_FONTS_H

#include <stdint.h>
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 12x12 点阵，汉字固定全宽，ASCII 变宽 */
extern const uint8_t cn_font_12[];
/** @brief 16x16 点阵，汉字固定全宽，ASCII 变宽 */
extern const uint8_t cn_font_16[];
/** @brief 24x24 点阵，汉字固定全宽，ASCII 变宽 */
extern const uint8_t cn_font_24[];
/** @brief 16x16 点阵，汉字固定全宽，ASCII 变宽 */
extern const uint8_t cn_font_16_bold[];

/* 配套用法：
 *   u8g2_SetFont(&g_u8g2, cn_font_16);
 *   u8g2_DrawUTF8(&g_u8g2, x, y, "系统设置");
 *   w = u8g2_GetUTF8Width(&g_u8g2, "系统设置");
 * 注意：u8g2_DrawStr / u8g2_GetStrWidth 只按 ASCII 解码，中文必须用 UTF8 版。
 */

#ifdef __cplusplus
}
#endif

#endif /* CN_FONTS_H */
