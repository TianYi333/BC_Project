/* u8g2_d_setup.c */
/* generated code, codebuild, u8g2 project */

#include "u8g2.h"

/* ssd1363 */
/* ssd1363 1 */
void u8g2_Setup_ssd1363_256x128_1(u8g2_t *u8g2, const u8g2_cb_t *rotation,
		u8x8_msg_cb byte_cb, u8x8_msg_cb gpio_and_delay_cb) {
	uint8_t tile_buf_height;
	uint8_t *buf;
	u8g2_SetupDisplay(u8g2, u8x8_d_ssd1363_256x128, u8x8_cad_011, byte_cb,
			gpio_and_delay_cb);
	buf = u8g2_m_32_16_1(&tile_buf_height);
	u8g2_SetupBuffer(u8g2, buf, tile_buf_height,
			u8g2_ll_hvline_vertical_top_lsb, rotation);
}
/* ssd1363 2 */
void u8g2_Setup_ssd1363_256x128_2(u8g2_t *u8g2, const u8g2_cb_t *rotation,
		u8x8_msg_cb byte_cb, u8x8_msg_cb gpio_and_delay_cb) {
	uint8_t tile_buf_height;
	uint8_t *buf;
	u8g2_SetupDisplay(u8g2, u8x8_d_ssd1363_256x128, u8x8_cad_011, byte_cb,
			gpio_and_delay_cb);
	buf = u8g2_m_32_16_2(&tile_buf_height);
	u8g2_SetupBuffer(u8g2, buf, tile_buf_height,
			u8g2_ll_hvline_vertical_top_lsb, rotation);
}
/* ssd1363 f */
void u8g2_Setup_ssd1363_256x128_f(u8g2_t *u8g2, const u8g2_cb_t *rotation,
		u8x8_msg_cb byte_cb, u8x8_msg_cb gpio_and_delay_cb) {
	uint8_t tile_buf_height;
	uint8_t *buf;
	u8g2_SetupDisplay(u8g2, u8x8_d_ssd1363_256x128, u8x8_cad_011, byte_cb,
			gpio_and_delay_cb);
	buf = u8g2_m_32_16_f(&tile_buf_height);
	u8g2_SetupBuffer(u8g2, buf, tile_buf_height,
			u8g2_ll_hvline_vertical_top_lsb, rotation);
}

/* end of generated code */
