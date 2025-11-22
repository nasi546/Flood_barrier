#ifndef __MY_LCD_I2C_H__
#define __MY_LCD_I2C_H__

#include "stm32f4xx_hal.h"

// LCD ?? (?? 0x27 ?? 0x3F ? ?? ??? ??)
#define LCD_I2C_ADDR (0x27 << 1)  // ??? ?? ? ?? ? 0x3F? ?? ??

// ?? ?? I2C ??? ?? (main.c? ??)
extern I2C_HandleTypeDef hi2c1;

// LCD ?? ??
void lcd_init(void);
void lcd_send_cmd(uint8_t cmd);
void lcd_send_data(uint8_t data);
void lcd_send_string(char *str);
void lcd_put_cur(uint8_t row, uint8_t col);
void lcd_clear(void);
void lcd_create_char(uint8_t location, uint8_t charmap[]);

#endif // __MY_LCD_I2C_H__
