#include "printing_page.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "board_conf.h"
#include "printer/send/send_cmd.h"
#include "ui/corner.h"
#include "ui/theme.h"
#include "user_conf.h"

namespace ui {
namespace printing_page {

static lv_obj_t *_fill = nullptr;
static lv_obj_t *_wave = nullptr;
static lv_timer_t *_wave_timer = nullptr;
static lv_obj_t *_pct = nullptr;

//: The strip of glass the surface moves through, painted directly.
//:
//: 4800 bytes of DRAM, and the reason the tide is affordable at all - see the
//: note on _paint_wave.
static const int32_t kWaveBand = 2 * WAVE_AMP;
static uint16_t _wave_buf[RES_H * kWaveBand];
static uint16_t _wave_fg = 0;
static uint16_t _wave_bg = 0;
static lv_obj_t *_sub = nullptr;
static lv_obj_t *_tool = nullptr;
static lv_obj_t *_dot_l = nullptr;
static lv_obj_t *_dot_r = nullptr;
static lv_obj_t *_scrim_pct = nullptr;
static lv_obj_t *_scrim_sub = nullptr;
static lv_obj_t *_scrim_tool = nullptr;
static lv_obj_t *_key_pause = nullptr;
static lv_obj_t *_key_cancel = nullptr;

//: Cancel asks twice. This is the timer that forgets the first ask, so a stray
//: tap cannot leave the control armed for the rest of the print.
static lv_timer_t *_cancel_timer = nullptr;
static bool _cancel_armed = false;
static bool _paused = false;

//: Extrusion rate off the wire, and the surface it drives.
//:
//: Amplitude is carried in sixteenths so it can ease rather than step - five
//: whole pixels of range would arrive in five visible jumps otherwise. Phase is
//: in 1/256ths of a table entry for the same reason.
static int32_t _flow = 0;
static int32_t _amp_fx = 0;
static int32_t _phase_fx = 0;
static int32_t _fill_top = -1;

//: Last values written, so an update that changes nothing touches nothing.
//: Impossible sentinels, so the first update after a rebuild always writes.
static uint32_t _colored = 0xFFFFFFFFu;
static int32_t _pct_shown = INT32_MIN;
static int32_t _sub_hot = INT32_MIN;
static int32_t _sub_target = INT32_MIN;
static int32_t _tool_shown = INT32_MIN;
static int8_t _dots_shown = -1;

//: Remembers the material the sub-line scrim was last sized for, so it is
//: measured again when the spool changes rather than on every packet.
//:
//: Cleared in init(). It outlives the object it describes - a status change
//: reloads the screen and every scrim on it is built anew - so a cache left
//: standing would report the fresh one as already sized and it would never get
//: a size at all.
static char _sized_for[printer::kFilamentTypeMaxLen + 1] = {0};

namespace {

//: One cycle in 32 steps, scaled to +/-100. Integer, like the heat ramp: this
//: runs eight times per tick and there is no reason for it to touch the FPU.
const int8_t kSine[32] = {
    0,   20,  38,  56,  71,  83,  92,  98,
    100, 98,  92,  83,  71,  56,  38,  20,
    0,   -20, -38, -56, -71, -83, -92, -98,
    -100, -98, -92, -83, -71, -56, -38, -20,
};

//: Phase advance per pixel across the glass, in 1/256ths of a table entry.
//: 51 puts about one and a half wavelengths on the screen - enough that the
//: surface is visibly travelling rather than heaving as one block.
const int32_t kWavePerPixel = 51;

//: Table entries per tick at full flow, in 1/256ths. About one cycle a second.
const int32_t kWaveSpeed = 307;

int32_t _sine(int32_t phase_fx) {
  // Wrapped rather than clamped, and masked rather than modulo'd, because the
  // phase runs backwards during a retraction and would otherwise index behind
  // the table.
  return kSine[(uint32_t)(phase_fx >> 8) & 31u];
}

}

static void _pause_handler(lv_event_t *e);
static void _cancel_handler(lv_event_t *e);
static void _wave_tick(lv_timer_t *timer);
static void _page_deleted(lv_event_t *e);
static void _place_wave();
static void _paint_wave();
static void _disarm_cancel(lv_timer_t *timer);
static void _show_cancel_state();
static lv_obj_t *_init_dot(lv_obj_t *parent);
static lv_obj_t *_init_scrim(lv_obj_t *parent);
static void _size_scrim(
    lv_obj_t *scrim, const lv_font_t *font, const char *widest,
    int32_t pad_x, int32_t pad_y, lv_align_t align, int32_t y);

lv_obj_t *init(lv_obj_t *parent, const printer::State &state) {
  lv_obj_t *page = lv_obj_create(parent);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, RES_H, RES_V);

  // The fill is a plain rectangle rising from the bottom, with no circular
  // mask: the GC9A01 is a round panel, so pixels outside the inscribed circle
  // do not exist on the glass. The hardware does the clipping for free, and a
  // full-screen mask every frame is exactly what this screen cannot afford.
  _fill = lv_obj_create(page);
  lv_obj_remove_style_all(_fill);
  lv_obj_remove_flag(_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_width(_fill, RES_H);
  lv_obj_set_height(_fill, 0);
  lv_obj_align(_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(_fill, LV_OPA_COVER, LV_PART_MAIN);

  // The surface: a strip of glass the width of the screen, painted a pixel at
  // a time. The fill stops WAVE_AMP short of true progress and this band spans
  // the WAVE_AMP either side of it, so a dead-flat surface lands exactly on the
  // waterline and progress stays honest whatever the swell is doing.
  //
  // A canvas rather than widgets. The first version of this was eight columns
  // whose heights were set each frame, which drew almost nothing - 14 kpx/s -
  // and still cost 44% of the UI task. Setting a widget's height is a style
  // write, a style write that affects geometry marks the layout dirty, and a
  // dirty layout is recalculated from the screen down - so every frame of the
  // wave was re-running the flex layout of the whole five-page row to move
  // eight rectangles by a pixel. Painting into a buffer touches no styles and
  // no layout, and invalidates one rectangle instead of sixteen.
  _wave = lv_canvas_create(page);
  lv_canvas_set_buffer(_wave, _wave_buf, RES_H, kWaveBand, LV_COLOR_FORMAT_RGB565);
  lv_obj_remove_flag(_wave, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(_wave, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(_wave, LV_ALIGN_BOTTOM_MID, 0, 0);

  _flow = 0;
  _amp_fx = 0;
  _phase_fx = 0;
  _fill_top = -1;

  // Every cache forgotten, for the same reason _sized_for is below: none of
  // these are part of the page, so they outlive it. Left standing they would
  // describe widgets that no longer exist, and the fresh ones would sit empty
  // until the printer happened to change.
  _colored = 0xFFFFFFFFu;
  _pct_shown = INT32_MIN;
  _sub_hot = INT32_MIN;
  _sub_target = INT32_MIN;
  _tool_shown = INT32_MIN;
  _dots_shown = -1;

  // Deleted with the page. Not lv_timer_set_repeat_count - that makes LVGL
  // delete the timer itself and leaves this pointer dangling, which is a
  // double free waiting for the next status change.
  lv_obj_add_event_cb(page, _page_deleted, LV_EVENT_DELETE, nullptr);
  _wave_timer = lv_timer_create(_wave_tick, WAVE_TICK_MS, nullptr);

  // Every readout gets a scrim - a soft dark pill sized to the text, drawn over
  // the fill and under the label.
  //
  // Flipping the ink black or white by luminance cannot work here. Each label
  // crosses the rising waterline at a different progress, and the 48px numeral
  // takes about a fifth of a print to cross it: for all of that it is half over
  // fill and half over black, where neither ink survives. A blend mode is no
  // better - difference inverts, and a mid-grey filament inverts to mid-grey
  // and disappears.
  //
  // Black on black is a no-op, so a scrim is invisible until the fill is
  // actually behind it. It costs nothing while the screen is dark and darkens
  // only the few hundred pixels the text needs.
  _scrim_tool = _init_scrim(page);
  _tool = lv_label_create(page);
  lv_obj_set_style_text_font(_tool, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(_tool, LV_ALIGN_TOP_MID, 0, 30);

  // The active marker is two drawn dots rather than characters around the
  // label. LVGL's Montserrat carries ASCII and its own symbols, so a middot
  // would render as a placeholder box - and flanking dots keep the label
  // optically centred instead of shoving it sideways when a tool goes active.
  _dot_l = _init_dot(page);
  _dot_r = _init_dot(page);

  _scrim_pct = _init_scrim(page);
  _pct = lv_label_create(page);
  lv_obj_set_style_text_font(_pct, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_align(_pct, LV_ALIGN_CENTER, 0, -6);

  _scrim_sub = _init_scrim(page);
  _sub = lv_label_create(page);
  lv_obj_set_style_text_font(_sub, &lv_font_montserrat_18, LV_PART_MAIN);
  // 48, not 34: at 34 the two scrims met at the same pixel and read as one
  // shape with a seam. This clears the percentage scrim by about 10px, with the
  // taller 18px line accounted for.
  lv_obj_align(_sub, LV_ALIGN_CENTER, 0, 48);

  // White throughout: the scrim guarantees the ground, so nothing has to be
  // chosen against the filament colour.
  lv_obj_set_style_text_color(_pct, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_color(_sub, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_color(_tool, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(_tool, LV_OPA_80, LV_PART_MAIN);

  // Fixed from here on. "100%" is the widest the percentage ever gets, and the
  // tool scrim is always sized as though the active dots were showing - a
  // slightly generous pill on an idle tool costs nothing, while resizing when a
  // tool takes over would be one more thing moving on screen.
  _size_scrim(_scrim_pct, &lv_font_montserrat_48, "100%", 14, 4, LV_ALIGN_CENTER, -6);
  _size_scrim(_scrim_tool, &lv_font_montserrat_16, "T00", 26, 4, LV_ALIGN_TOP_MID, 30);

  // The sub-line scrim is sized from the material, so it is the one printer_update
  // owns. Forget what the previous incarnation of this page was showing, or the
  // scrim created three lines above never gets measured.
  _sized_for[0] = '\0';

  // The two controls worth having during a print, at the two lower diagonals -
  // which is where the physical keys go, so these are already in the right
  // place to become their legends. Created last, so they sit over the fill and
  // the scrims.
  _paused = state.paused;
  _cancel_armed = false;
  _key_pause = corner::create(
      page, corner::Slot::kSW, LV_SYMBOL_PAUSE, COLOR_PAUSE_BG, _pause_handler);
  _key_cancel = corner::create(
      page, corner::Slot::kSE, LV_SYMBOL_STOP, COLOR_CANCEL_BG, _cancel_handler);

  printer_update(state);
  return page;
}

static void _pause_handler(lv_event_t *e) {
  printer::send::send_gcode(_paused ? "RESUME" : "PAUSE");
}

static void _page_deleted(lv_event_t *e) {
  if (_wave_timer) {
    lv_timer_delete(_wave_timer);
    _wave_timer = nullptr;
  }
  _wave = nullptr;
}

//: Sit the band back on top of the fill after progress moves it. Only when
//: progress actually moves - this one does mark the layout dirty, which is
//: affordable once a percent and was not affordable thirty times a second.
static void _place_wave() {
  if (_wave) {
    lv_obj_align(_wave, LV_ALIGN_BOTTOM_MID, 0, -_fill_top);
  }
}

// Paint one frame of the surface into the canvas buffer.
//
// Rows outer and columns inner, so each row is a straight walk through memory.
// 2400 pixels a frame, which is a twenty-fourth of the glass.
static void _paint_wave() {
  if (!_wave) {
    return;
  }

  static uint8_t surface[RES_H];
  for (int32_t x = 0; x < RES_H; x++) {
    int32_t s = _sine(_phase_fx + x * kWavePerPixel);
    int32_t top = WAVE_AMP - (_amp_fx * s) / (16 * 100);
    if (top < 0) {
      top = 0;
    }
    if (top > kWaveBand) {
      top = kWaveBand;
    }
    surface[x] = (uint8_t)top;
  }

  for (int32_t y = 0; y < kWaveBand; y++) {
    uint16_t *row = _wave_buf + y * RES_H;
    for (int32_t x = 0; x < RES_H; x++) {
      // Above the surface is the page's own ground, which is black - the same
      // thing that would show if this band were not here at all.
      row[x] = ((int32_t)surface[x] > y) ? _wave_bg : _wave_fg;
    }
  }

  lv_obj_invalidate(_wave);
}

// The surface, once per LVGL refresh.
//
// Amplitude follows the extrusion rate and eases rather than snapping, so a
// travel move does not slam the sea flat and back. Phase advances at a rate set
// by the same number and signed by it: a retraction runs the swell backwards,
// which is a small thing nobody will consciously notice and exactly what a real
// surface does when you pull filament back up the tube.
static void _wave_tick(lv_timer_t *timer) {
  // -256..256, the flow as a fraction of what counts as full.
  int32_t flow_fx = _flow * 256 / WAVE_FLOW_FULL;
  if (flow_fx > 256) {
    flow_fx = 256;
  }
  if (flow_fx < -256) {
    flow_fx = -256;
  }

  int32_t magnitude = flow_fx < 0 ? -flow_fx : flow_fx;
  int32_t target_fx = magnitude * WAVE_AMP * 16 / 256;
  int32_t was_fx = _amp_fx;
  // An eighth of the remaining distance each tick: about a fifth of a second to
  // settle, fast enough to answer a change in flow and slow enough not to
  // flicker on the gaps between moves.
  _amp_fx += (target_fx - _amp_fx) / 8;
  if (_amp_fx != target_fx && (target_fx - _amp_fx) / 8 == 0) {
    _amp_fx += target_fx > _amp_fx ? 1 : -1;
  }

  // A flat surface that was already flat is the idle case, and it must cost
  // nothing at all - this page sits at 28% while printing and there is no
  // reason for a finished job to keep paying for waves it is not making.
  if (_amp_fx == 0 && was_fx == 0) {
    return;
  }

  _phase_fx += flow_fx * kWaveSpeed / 256;
  _paint_wave();
}

// Two taps, because this ends a job. The first arms and says so by turning into
// a tick; the second commits.
//
// This is the graceful one - Klipper finishes its move, runs the cancel macro
// and parks - so nothing is gained by it being instant. The e-stop next door
// asks twice as well, but for the opposite reason: it stops the machine where
// it stands to limit damage, and is guarded as lightly as it can stand rather
// than as heavily as it can bear.
static void _cancel_handler(lv_event_t *e) {
  if (!_cancel_armed) {
    _cancel_armed = true;
    _show_cancel_state();
    if (!_cancel_timer) {
      _cancel_timer = lv_timer_create(_disarm_cancel, CONFIRM_MS, nullptr);
      lv_timer_set_repeat_count(_cancel_timer, 1);
    }
    return;
  }
  _cancel_armed = false;
  _show_cancel_state();
  printer::send::send_gcode("CANCEL_PRINT");
}

static void _disarm_cancel(lv_timer_t *timer) {
  _cancel_timer = nullptr;
  _cancel_armed = false;
  _show_cancel_state();
}

static void _show_cancel_state() {
  if (!_key_cancel) {
    return;
  }
  corner::set(
      _key_cancel,
      _cancel_armed ? LV_SYMBOL_OK : LV_SYMBOL_STOP,
      _cancel_armed ? COLOR_CONFIRM_BG : COLOR_CANCEL_BG);
}

static lv_obj_t *_init_scrim(lv_obj_t *parent) {
  lv_obj_t *scrim = lv_obj_create(parent);
  lv_obj_remove_style_all(scrim);
  lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
  // Start at nothing. An lv_obj defaults to LV_DPI_DEF square at the top-left
  // corner, so a scrim that misses its sizing draws a large circle in the upper
  // left rather than not drawing - which is how this went unnoticed. Failing
  // invisibly is the right failure for a mask.
  lv_obj_set_size(scrim, 0, 0);
  lv_obj_set_style_radius(scrim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(scrim, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scrim, SCRIM_OPA, LV_PART_MAIN);
  return scrim;
}

// Sized to the widest string the label will ever hold, not to what it holds
// right now. Montserrat is proportional, so measuring live made the scrim
// breathe on every digit - "9%" to "10%" to "100%" - which draws the eye to the
// mask instead of the number. The label stays centred inside a scrim that does
// not move, so only the digits change.
//
// Both are aligned to the same anchor rather than the scrim being aligned to
// the label, which keeps them concentric whatever the text measures and drops a
// forced layout pass out of the update path.
static void _size_scrim(
    lv_obj_t *scrim, const lv_font_t *font, const char *widest,
    int32_t pad_x, int32_t pad_y, lv_align_t align, int32_t y) {
  lv_point_t size;
  lv_text_get_size(&size, widest, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

  // filament_type allows 15 characters. Nothing real is longer than "NYLON",
  // but an odd one would otherwise stretch the pill past the edge of the glass.
  int32_t w = size.x + pad_x * 2;
  if (w > RES_H - 30) {
    w = RES_H - 30;
  }
  lv_obj_set_size(scrim, w, size.y + pad_y * 2);
  if (align == LV_ALIGN_TOP_MID) {
    lv_obj_align(scrim, align, 0, y - pad_y);
  } else {
    lv_obj_align(scrim, align, 0, y);
  }
}

static lv_obj_t *_init_dot(lv_obj_t *parent) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 5, 5);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, theme::machine(), LV_PART_MAIN);
  return dot;
}

void printer_update(const printer::State &state) {
  if (state.paused != _paused) {
    _paused = state.paused;
    corner::set(
        _key_pause,
        _paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE,
        _paused ? COLOR_RESUME_BG : COLOR_PAUSE_BG);
  }

  _flow = state.flow;

  // Guarded, because setting a local style property invalidates the object
  // whether or not the value changed - and this one is the full width of the
  // glass. It was being written ten times a second to say the filament was
  // still the colour it had been all print.
  if (state.filament_color != _colored) {
    _colored = state.filament_color;
    lv_color_t colour = theme::filament(state);
    lv_obj_set_style_bg_color(_fill, colour, LV_PART_MAIN);
    _wave_fg = lv_color_to_u16(colour);
    _wave_bg = lv_color_to_u16(lv_color_black());
    _paint_wave();
  }

  int32_t pct = state.progress;
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }

  // The body of the fill stops WAVE_AMP short and the columns make up the
  // difference, so the average surface is exactly where progress says it is.
  // Clamped at both ends: nothing to stand on at zero, and nothing above the
  // glass at a hundred.
  int32_t top = pct * RES_V / 100 - WAVE_AMP;
  if (top < 0) {
    top = 0;
  }
  if (top > RES_V - 2 * WAVE_AMP) {
    top = RES_V - 2 * WAVE_AMP;
  }
  if (top != _fill_top) {
    _fill_top = top;
    lv_obj_set_height(_fill, top);
    _place_wave();
  }

  if (pct != _pct_shown) {
    _pct_shown = pct;
    lv_label_set_text_fmt(_pct, "%d%%", (int)pct);
  }

  if (state.hotend_temp != _sub_hot || state.hotend_target != _sub_target ||
      strncmp(_sized_for, state.filament_type, sizeof(_sized_for) - 1) != 0) {
    _sub_hot = state.hotend_temp;
    _sub_target = state.hotend_target;

    if (state.filament_type[0] != '\0') {
      lv_label_set_text_fmt(_sub, "%s   %d", state.filament_type, (int)_sub_hot);
    } else {
      lv_label_set_text_fmt(_sub, "%d", (int)_sub_hot);
    }

    // The readout takes the heat colour while something is being asked of the
    // heater, and plain ink when nothing is. The scrim guarantees a dark ground
    // underneath, so even the cool end of the ramp stays legible.
    lv_obj_set_style_text_color(
        _sub,
        _sub_target > 0 ? theme::heat_ink(_sub_hot, _sub_target)
                        : lv_color_white(),
        LV_PART_MAIN);
  }

  if (state.tool_number != _tool_shown) {
    _tool_shown = state.tool_number;
    if (_tool_shown >= 0) {
      lv_label_set_text_fmt(_tool, "T%d", (int)_tool_shown);
      lv_obj_remove_flag(_tool, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(_scrim_tool, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(_tool, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(_scrim_tool, LV_OBJ_FLAG_HIDDEN);
    }
    // Only here. The tag is the one label whose width can change without its
    // text changing every packet, so this is the only place the dots can have
    // moved - it used to run on every update, dirtying the layout to put them
    // back exactly where they already were.
    lv_obj_align_to(_dot_l, _tool, LV_ALIGN_OUT_LEFT_MID, -7, 1);
    lv_obj_align_to(_dot_r, _tool, LV_ALIGN_OUT_RIGHT_MID, 7, 1);
  }

  int8_t show_dots = (state.active && state.tool_number >= 0) ? 1 : 0;
  if (show_dots != _dots_shown) {
    _dots_shown = show_dots;
    if (show_dots) {
      lv_obj_remove_flag(_dot_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(_dot_r, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(_dot_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(_dot_r, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The sub-line is the one width that genuinely varies, because the material
  // name does. Measured against a three-digit temperature so the digits never
  // move it, and only re-measured when the spool actually changes - which is
  // once a print, not ten times a second.
  if (strncmp(_sized_for, state.filament_type, sizeof(_sized_for) - 1) != 0) {
    strncpy(_sized_for, state.filament_type, sizeof(_sized_for) - 1);
    _sized_for[sizeof(_sized_for) - 1] = '\0';

    char widest[printer::kFilamentTypeMaxLen + 8];
    if (_sized_for[0] != '\0') {
      snprintf(widest, sizeof(widest), "%s   888", _sized_for);
    } else {
      snprintf(widest, sizeof(widest), "888");
    }
    _size_scrim(_scrim_sub, &lv_font_montserrat_18, widest, 12, 3, LV_ALIGN_CENTER, 48);
  }
}

}
}
