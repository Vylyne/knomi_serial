#ifndef USER_CONF_H
#define USER_CONF_H

// Display options.
#define PRINTER_NAME "Knomi_Serial"
#define DISPLAY_BRIGHTNESS 8

// Display sleep settings.
#define SLEEP_DIM_BRIGHTNESS 3
#define SLEEP_DIM_MS 30000     // dim after 30s
#define SLEEP_TIMEOUT_MS 60000 // off after 60s
#define SLEEP_HOT_THRESHOLD 40 // degrees C

// Overlay side indicators.
#define SHOW_SIDE_ARCS 1

// Idle screen page order.
#define IDLE_PAGE_0 gcode_page
#define IDLE_PAGE_1 temp_page
#define IDLE_PAGE_2 filament_page
#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
#define IDLE_PAGE_3 home_page
#define IDLE_PAGE_4 move_page
#endif

// Index of default idle screen page.
#define IDLE_PAGE_START 1

// All screens background color.
#define COLOR_BG lv_color_hex(0x000000)

// Overlay colors.
#define COLOR_STOP_BG lv_color_hex(0x900000)
#define COLOR_SIDE_ARC lv_color_hex(0x444444)
#define COLOR_PROGRESS_ARC lv_color_hex(0x238932)

// Button colors.
#define COLOR_BTN_BG lv_color_hex(0xffbaf8)
#define COLOR_PAUSE_BG lv_color_hex(0xbaffc1)
#define COLOR_RESUME_BG lv_color_hex(0xbaffe3)
#define COLOR_CANCEL_BG lv_color_hex(0x900000)
#define COLOR_HOMED_BG lv_color_hex(0xffe3ba)

// G-code screen colors.
#define COLOR_GCODE_UNSELECTED lv_color_hex(0x808080)
#define COLOR_GCODE_HIGHLIGHT lv_color_hex(0x444444)

#define SERIAL_BAUD_RATE 115200
#define SERIAL_TIMEOUT 1000

#endif
