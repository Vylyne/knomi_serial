#ifndef TFT_SETUP_H
#define TFT_SETUP_H

// TFT_eSPI user setup for Knomi V2. See docs/hardware.md.

#define USER_SETUP_LOADED
#define DISABLE_ALL_LIBRARY_WARNINGS

// Driver and display dimensions
#define GC9A01_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 240

// Include board pin definitions
#include "knomi_v2.h"

// Panel SPI pins from knomi_v2.h
#define TFT_MOSI LCD_MOSI
#define TFT_SCLK LCD_SCLK
#define TFT_CS LCD_CS
#define TFT_DC LCD_DC
#define TFT_RST LCD_RST

// Display configuration
#define TFT_INVERSION_ON

// SPI frequencies
#define SPI_FREQUENCY 80000000
#define SPI_READ_FREQUENCY 5000000

#endif
