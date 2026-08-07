#include "overlay.h"

// LVGL 9.3 made lv_hit_test_info_t opaque in the public headers. Unlike
// LV_EVENT_COVER_CHECK, which grew lv_event_set_cover_res(), the hit test has
// no public setter - the struct has to be written directly, so the private
// header is the only way to reach `res`.
#include <core/lv_obj_event_private.h>

#include "board_conf.h"
#include "printer/send/send_cmd.h"
#include "ui/pages/page_helper.h"
#include "ui/ui.h"
#include "user_conf.h"

namespace ui
{
  namespace overlay
  {

    lv_obj_t *_init_arc(int start, int end);
    void _arc_hit_handler(lv_event_t *e);

    void _printer_update_handler(const printer::State &state);

    // Blocking Stop Button in Toolchanger Mode
#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
    void _stop_click_handler(lv_event_t *e);
#endif

    void _right_click_handler(lv_event_t *e);
    void _left_click_handler(lv_event_t *e);

    lv_obj_t *init(const printer::State &state)
    {
      lv_obj_t *scr = lv_obj_create(nullptr);
      control::register_printer_update_cb(scr, _printer_update_handler);

      // No progress arc here any more. It swept 300 degrees from the lower left
      // up over the top, in a green belonging to no colour role, and since the
      // printing screen became the fill it was saying a second time what the
      // fill already says - on top of it, across the part of the screen the fill
      // is densest. The control page carries its own readout instead, which is
      // the only place the arc was still earning its keep.

      // Blocking Stop Button in Toolchanger Mode
#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
      lv_obj_t *stop = lv_button_create(lv_layer_top());
      lv_obj_set_size(stop, RES_H, 35);
      lv_obj_align(stop, LV_ALIGN_BOTTOM_MID, 0, 0);
      lv_obj_t *stop_label = lv_label_create(stop);
      lv_label_set_text(stop_label, "STOP");
      lv_obj_center(stop_label);
      page_helper::set_button_color(stop, COLOR_STOP_BG);

      lv_obj_add_event_cb(
          stop,
          _stop_click_handler,
          LV_EVENT_CLICKED,
          nullptr);
#endif

      lv_obj_t *right = _init_arc(350, 10);
      lv_obj_add_event_cb(
          right,
          _right_click_handler,
          LV_EVENT_CLICKED,
          nullptr);

      lv_obj_t *left = _init_arc(170, 190);
      lv_obj_add_event_cb(
          left,
          _left_click_handler,
          LV_EVENT_CLICKED,
          nullptr);

      return scr;
    }

    lv_obj_t *_init_arc(int start, int end)
    {
      lv_obj_t *obj = lv_arc_create(lv_layer_top());
      lv_obj_set_width(obj, RES_H - 20);
      lv_obj_set_height(obj, RES_V - 20);
      lv_obj_set_align(obj, LV_ALIGN_CENTER);
      lv_arc_set_bg_angles(obj, start, end);
      lv_obj_set_style_arc_color(obj, COLOR_SIDE_ARC, LV_PART_MAIN);
      lv_obj_set_style_arc_width(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
      lv_obj_set_style_arc_width(obj, 8, LV_PART_MAIN | LV_STATE_PRESSED);
#if SHOW_SIDE_ARCS == 0
      lv_obj_set_style_opa(obj, 0, LV_PART_MAIN);
#endif
      lv_obj_set_style_opa(obj, 0, LV_PART_INDICATOR);
      lv_obj_set_style_opa(obj, 0, LV_PART_KNOB);
      lv_obj_add_flag(obj, LV_OBJ_FLAG_ADV_HITTEST);
      lv_obj_add_event_cb(
          obj,
          _arc_hit_handler,
          LV_EVENT_HIT_TEST,
          nullptr);
      return obj;
    }

    void _arc_hit_handler(lv_event_t *e)
    {
      lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
      lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
      if (lv_arc_get_bg_angle_end(obj) < 180)
      {
        info->res = info->point->x > RES_H - 30;
      }
      else
      {
        info->res = info->point->x < 30;
      }
    }

    void _printer_update_handler(const printer::State &state)
    {
      if (state.status == printer::Status::kDisconnected ||
          state.status == printer::Status::kShutdown)
      {
        lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
      }
      else
      {
        lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
      }

    }

    // Blocking Stop Button in Toolchanger Mode
#if !defined(TOOLCHANGER) || TOOLCHANGER == 0
    void _stop_click_handler(lv_event_t *e)
    {
      printer::send::send_stop();
    }
#endif

    void _right_click_handler(lv_event_t *e)
    {
      control::scroll_right();
    }

    void _left_click_handler(lv_event_t *e)
    {
      control::scroll_left();
    }

  }
}