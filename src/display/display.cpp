#include "display.h"

#include <lvgl.h>
#include <TFT_eSPI.h>

#include "board_conf.h"
#include "cst816s.h"

namespace display {

static TFT_eSPI *_tft = nullptr;
static lv_display_t *_lvd = nullptr;

static lv_color_t _buf1[RES_H * RES_V / 10];
static lv_color_t _buf2[RES_H * RES_V / 10];

static lv_indev_t *_indev = nullptr;

static volatile uint32_t _flush_count = 0;
static volatile uint32_t _flush_px = 0;
static volatile uint32_t _flush_us = 0;

static uint16_t *_capture = nullptr;
static volatile bool _capturing = false;
static volatile uint32_t _capture_px = 0;

void _flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *color);
void _read_touchscreen(lv_indev_t *indev, lv_indev_data_t *data);

void init() {
  pinMode(LCD_BL_PIN, OUTPUT);

  _tft = new TFT_eSPI();
  _tft->init();
  set_backlight(0);

  _lvd = lv_display_create(RES_H, RES_V);
  lv_display_set_buffers(_lvd, _buf1, _buf2, sizeof(_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(_lvd, _flush_display);

  cst816s::init(); 

  _indev = lv_indev_create();
  lv_indev_set_type(_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(_indev, _read_touchscreen);

  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), LV_STATE_DEFAULT);
}

void destroy() {
  lv_indev_delete(_indev);
  _indev = nullptr;

  lv_display_delete(_lvd);
  _lvd = nullptr;

  delete _tft;
  _tft = nullptr;
}

void set_backlight(uint8_t target) {
  static uint8_t current = 0;
  if (target > 16) {
    target = 16;
  }

  if (target == current) {
    return;
  }

  if (target == 0) {
    digitalWrite(LCD_BL_PIN, LOW);
    delay(3);
    current = 0;
    return;
  }

  if (current == 0) {
    digitalWrite(LCD_BL_PIN, HIGH);
    delayMicroseconds(25);
    current = 16;
  }

  uint8_t cycles = (current >= target) ? current - target : current + 16 - target;
  for (uint8_t i = 0; i < cycles; i ++) {
    digitalWrite(LCD_BL_PIN, LOW);
    delayMicroseconds(1);
    digitalWrite(LCD_BL_PIN, HIGH);
    delayMicroseconds(1);
  }

  current = target;
}

bool capture_begin() {
  if (!_capture) {
    _capture = (uint16_t *)ps_malloc((size_t)RES_H * RES_V * sizeof(uint16_t));
  }
  if (!_capture) {
    return false;
  }
  _capture_px = 0;
  _capturing = true;
  // Nothing may have changed on screen for minutes, and LVGL only renders what
  // is dirty - so without this a capture of a still screen would collect
  // nothing at all and never complete.
  lv_obj_invalidate(lv_screen_active());
  return true;
}

bool capture_complete() {
  return _capturing && _capture_px >= (uint32_t)RES_H * RES_V;
}

const uint16_t *capture_frame() {
  return _capture;
}

void capture_end() {
  _capturing = false;
  if (_capture) {
    free(_capture);
    _capture = nullptr;
  }
}

void _flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *color) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  if (_capturing && _capture) {
    // Row by row, because a flush area is a strip of the screen and its rows
    // are contiguous only within that strip.
    const uint16_t *src = (const uint16_t *)color;
    for (uint32_t row = 0; row < h; row++) {
      memcpy(_capture + (area->y1 + row) * RES_H + area->x1, src, w * 2);
      src += w;
    }
    _capture_px += w * h;
  }

  uint32_t enter = micros();

  _tft->startWrite();
  _tft->setAddrWindow(area->x1, area->y1, w, h);
  _tft->pushColors((uint16_t*) color, w * h, true);
  _tft->endWrite();

  // Pixels actually pushed over SPI is the honest measure of what a screen
  // costs - a whole frame is 57600 of them, and at this panel's clock that
  // alone is over 11ms.
  _flush_count++;
  _flush_px += w * h;
  _flush_us += micros() - enter;

  lv_disp_flush_ready(display);
}

void take_flush_stats(uint32_t *count, uint32_t *pixels, uint32_t *micros_spent) {
  *count = _flush_count;
  *pixels = _flush_px;
  *micros_spent = _flush_us;
  _flush_count = 0;
  _flush_px = 0;
  _flush_us = 0;
}

void _read_touchscreen(lv_indev_t *indev, lv_indev_data_t *data) {
  if (!cst816s::ready()) {
    return;
  }

  bool pressed = cst816s::read(data->point.x, data->point.y);
  if (pressed) {
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

}