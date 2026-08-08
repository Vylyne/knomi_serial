#ifndef USER_CONF_H
#define USER_CONF_H

// Display options.
#define PRINTER_NAME "Knomi_Serial"
#define DISPLAY_BRIGHTNESS 8

// Display sleep settings.
#define SLEEP_DIM_BRIGHTNESS 3
#define SLEEP_DIM_MS 30000     // dim after 30s
#define SLEEP_TIMEOUT_MS 60000 // off after 60s
// Safety net only - the host's `used` flag decides whether a screen sleeps.
// Must sit above chamber temperature, or a tool idling at chamber heat reads as
// busy and the screen never sleeps at all.
#define SLEEP_HOT_THRESHOLD 80 // degrees C

// How often to report firmware version and device state back to the host.
#define REPORT_PERIOD_MS 2000

// Corner controls sit at the four diagonals, where the physical keys are going.
// With keys fitted, set CORNER_KEYS_TOUCH to 0: the symbols stay exactly where
// they are and become legends for the keys instead of touch targets.
#define CORNER_KEYS_TOUCH 1
#define CORNER_SIZE 52

// How long the e-stop must be held. Long enough that a brush cannot fire it,
// short enough to be no obstacle when meant. A hold, not a confirmation
// dialogue - an emergency stop should never wait for a second tap.
#define ESTOP_HOLD_MS 1500

// Cancelling a print asks twice, and forgets the first ask after this. Unlike
// the e-stop this ends a job rather than the machine, so a moment to reconsider
// is worth more than the speed is.
#define CANCEL_CONFIRM_MS 3000

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

// ---------------------------------------------------------------------------
// Colour
//
// Three roles that must not be collapsed into each other - see ui/theme.h.
// Machine identity is fixed chrome, filament comes off the wire per tool, and
// heat is a quantity. Anything below that is not one of those three is a page
// detail rather than part of the system.
// ---------------------------------------------------------------------------

// Machine identity. The printer's own accent, worn by chrome that is about the
// machine rather than the print.
#define COLOR_MACHINE 0xFFA7C4

// Shown when the host has not said what is loaded. Deliberately not black -
// unknown filament and black filament are different facts.
#define COLOR_FILAMENT_UNKNOWN 0x5A5A5A

// Heat shows as a gradient rising behind the fill. Strength at target, and how
// many rungs the ramp is quantised into - restyling the gradient invalidates
// the whole screen, so it must not follow every degree. More rungs is smoother
// and costs a full repaint each.
#define HEAT_HAZE_OPA 110
#define HEAT_STEPS 24

// How much white is mixed into the heat ramp when it is used as text, 0-255.
// The glow's cool end is a deep steel that is close to unreadable at small
// sizes; this lifts it without touching the haze.
#define HEAT_INK_LIFT 90

// Readouts sit on a scrim - a dark pill sized to the text - so they never have
// to be legible against the filament colour directly. Black over black is a
// no-op, so this is invisible until the fill is actually behind it. Raise it if
// pale filaments still crowd the text, lower it to let more colour through.
#define SCRIM_OPA LV_OPA_60

// All screens background color.
#define COLOR_BG lv_color_hex(0x000000)

// Overlay colors.
#define COLOR_STOP_BG lv_color_hex(0x900000)
#define COLOR_SIDE_ARC lv_color_hex(0x444444)

// Button colors.
#define COLOR_BTN_BG lv_color_hex(0xffbaf8)
#define COLOR_PAUSE_BG lv_color_hex(0xbaffc1)
#define COLOR_RESUME_BG lv_color_hex(0xbaffe3)
#define COLOR_CANCEL_BG lv_color_hex(0x900000)
// Shown while a cancel is armed and waiting for its second tap.
#define COLOR_CONFIRM_BG lv_color_hex(0xE04A32)
#define COLOR_HOMED_BG lv_color_hex(0xffe3ba)

// G-code screen colors.
#define COLOR_GCODE_UNSELECTED lv_color_hex(0x808080)
#define COLOR_GCODE_HIGHLIGHT lv_color_hex(0x444444)

#define SERIAL_BAUD_RATE 115200
#define SERIAL_TIMEOUT 1000

#endif
