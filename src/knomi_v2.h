#ifndef KNOMI_V2_H
#define KNOMI_V2_H

// Knomi V2 board pin definitions. See docs/hardware.md.

// I2C bus 0 (touch panel and external port)
#define I2C0_SCL 1
#define I2C0_SDA 2
#define I2C0_FREQ 100000

// Touch controller (CST816S)
#define TP_I2C_ADDR 0x15
#define TP_INT 17
#define TP_RST 16

// Panel SPI (GC9A01)
#define LCD_MOSI 14
#define LCD_SCLK 18
#define LCD_CS 20
#define LCD_DC 19
#define LCD_RST 21

// Backlight (AW9364 enable)
#define BACKLIGHT 12

#endif
