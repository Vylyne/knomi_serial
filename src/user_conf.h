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

// ---------------------------------------------------------------------------
// A link that has gone quiet
//
// The host sends every 100ms and there is nothing in the protocol that says
// goodbye - a Klipper that is killed rather than closed simply stops. Without a
// watchdog the screen holds its last frame forever, and a print that finished
// an hour ago is indistinguishable from one at 55%.
//
// Deliberately not a screen change. What was last heard is still the most
// useful thing to show; it just has to stop claiming to be current.
// ---------------------------------------------------------------------------

// Silence longer than this and the link is treated as stale. Thirty missed
// packets, so ordinary jitter or a busy host never trips it.
#define STALE_TIMEOUT_MS 3000

#define STALE_MARK_SIZE 18
// Above the pool rather than below the readout. It was at the very bottom,
// which forced the tool page's pool deeper and its material name up to clear
// it - a whole page's proportions bent around a mark that is only there when
// something has gone wrong.
//
// This band is genuinely empty on every page: the corner marks sit either side
// of it on the diagonals, and nothing is ever drawn down the middle between
// them.
#define STALE_MARK_Y -40
#define COLOR_STALE 0xE07A5F

// Corner controls sit at the four diagonals, where the physical keys go.
//
// Each corner is either a soft key - the glass is the button - or a legend for
// something else that already reports the press: a switch wired to the device,
// or a [gcode_button] on the host. A legend keeps its symbol in exactly the
// same place and loses only the touch target, so nothing on screen moves when
// hardware arrives and the position is already learned by then.
//
// This is the compile-time default. printer.cfg overrides it per corner with
// `hardware_keys`, because which corners have switches behind them is a fact
// about one machine, not about the firmware.
//
//   bit 0  NW    bit 1  NE    bit 2  SW    bit 3  SE
//
// 0 means every corner is soft, which is right for a bare display.
#define CORNER_LEGEND_KEYS 0

// Which side of the page row the emergency stop hangs off, so it is one swipe
// from every page rather than up to four along the row.
//
// kBottom means drag up to reach it; kTop is the notification-shade gesture,
// drag down. Overridden by `estop_at:` in printer.cfg, because which of those
// reads as obvious is a fact about a person and not about the firmware.
#define ESTOP_AT printer::EstopAt::kBottom

// 38 on a 70 offset. At 52 on 62 the corner circle and the readout pill below
// genuinely intersected - centres 24.8px apart against 39.5px of combined
// radii. 70 is also the furthest the offset can go: a 19px radius at
// 70*sqrt(2) from the middle puts the outer edge at 118 of the 120 available.
#define CORNER_SIZE 38
#define CORNER_OFFSET 70

// The hit area is far larger than the mark. A legend should be small and quiet;
// a target should be easy to hit with a gloved thumb. Decoupling them means
// fitting physical keys removes only the region - the mark stays exactly where
// it was, saying what the key beside it does.
//
// Square, because the glass is round: a short wide box hugs the bottom edge,
// where the circle has already curved away, and never reaches the mark sitting
// up on the diagonal. Equal sides put the target under the symbol.
//
// It does now pass behind the readout. Nothing there is touchable, so the only
// consequence is that a tap on the material or temperature counts as the
// control on that side - and the gap down the middle keeps the centre dead.
#define CORNER_TOUCH_W 106
#define CORNER_TOUCH_H 106
#define CORNER_TOUCH_INSET 4

// How faint a corner goes when its action is unavailable. Faint enough to read
// as off, present enough that the key beside it still has a legend.
#define CORNER_DISABLED_OPA LV_OPA_30

// Destructive controls ask twice and forget the first ask after this.
//
// Not a press-and-hold. A hold reads as safer but is slower exactly when speed
// matters, and this is not the machine's real emergency stop in any case - that
// belongs in hardware, on a latching switch that cuts power rather than asking
// a display to ask a host to ask an MCU.
#define CONFIRM_MS 3000

// Idle screen pages, in order, until Page::kNone. The screen lands on the
// first, so this chooses both the sequence and where you start. The e-stop page
// is appended whatever this says.
//
// This is the compile-time default; `pages:` in printer.cfg overrides it, the
// same way the colours and timings work. It is a runtime choice because the
// cost of a page you do not want is one more swipe rather than any measurable
// resource - see printer::Page.
//
// tool_page is temp_page and filament_page merged. They were split along a line
// the machine does not have: one listed every temperature and could not be
// touched, the other offered load and unload while saying nothing about what
// was loaded or whether it was warm enough to move. One page answers both.
#define DEFAULT_PAGES                                                  \
  {                                                                    \
    printer::Page::kTool, printer::Page::kGcode, printer::Page::kHome, \
        printer::Page::kMove, printer::Page::kNone,                    \
  }

// Secondary readouts beside the hotend, in order, until Readout::kNone.
// Overridden by `readouts:` in printer.cfg.
//
// Bed and chamber suit a single-toolhead machine. A toolchanger usually wants
// `readouts: mcu` instead - four screens each restating the one bed temperature
// is four copies of something none of them owns, while the MCU inside the
// chamber is a reading only that tool can give.
#define DEFAULT_READOUTS                                \
  {                                                     \
    printer::Readout::kBed, printer::Readout::kChamber, \
        printer::Readout::kNone,                        \
  }

// How the hotend's target is drawn beside it: small and dim, hung off the right
// of the number without moving it.
//
// The number must stay put whether or not there is a target - it is what the
// eye anchors on, and centring the pair would slide it sideways every time a
// heater was set or cleared.
#define TARGET_OPA LV_OPA_50
#define TARGET_GAP 4
#define TARGET_LIFT 9

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

// ---------------------------------------------------------------------------
// The tide
//
// The surface of the fill moves with the extruder. Not decoration for its own
// sake: it is the one thing on the screen that says the machine is working
// right now, as opposed to having been left partway through a job. Progress
// answers that too, but only once a minute.
// ---------------------------------------------------------------------------

// Peak height of the swell above the flat waterline, in pixels. The fill sits
// this far below true progress and the painted band puts it back, so the
// average surface is always exactly where progress says it is.
//
// Also half the height of the canvas, so raising it costs 480 bytes a pixel
// and a proportional share of each frame.
#define WAVE_AMP 8

// How fast the swell travels at full flow, in 1/256ths of a wave table entry
// per tick. 32 entries is one full cycle, so 190 is about one cycle every one
// and a half seconds.
//
// This is the speed at WAVE_FLOW_FULL and nothing prints there for long -
// ordinary perimeter flow sits around three quarters of it, and the wave slows
// with the extruder. Judge it against a real print rather than a pinned test.
//
// Slower is also cheaper: the surface is quantised to whole pixels, so a lazy
// wave spends several ticks describing the same shape and those frames are
// skipped before anything is painted.
#define WAVE_SPEED 190

// Extrusion rate that counts as a full-amplitude swell, in micrometres of
// filament per second. Around 3mm/s is ordinary perimeter flow.
#define WAVE_FLOW_FULL 3500

// How often the surface is recomputed. 33ms matches LVGL's own refresh period,
// so a faster tick would compute frames nobody sees.
#define WAVE_TICK_MS 33

// Depth of the pool of loaded filament along the bottom of the tool page - the
// printing page's fill, at rest. Deep enough to read as a body of colour, shallow
// enough to stay clear of the corner marks at 150-188.
// Deep enough to read as a body of colour, shallow enough to stay clear of the
// corner marks at 150-188. The material name has to sit wholly inside it: the
// name's ink is chosen against the pool's colour, so half a line of black text
// on black glass is not a readout.
#define POOL_H 36

// Readouts sit on a scrim - a dark pill sized to the text - so they never have
// to be legible against the filament colour directly. Black over black is a
// no-op, so this is invisible until the fill is actually behind it. Raise it if
// pale filaments still crowd the text, lower it to let more colour through.
#define SCRIM_OPA LV_OPA_60

// All screens background color.
#define COLOR_BG lv_color_hex(0x000000)

// Overlay colors.
#define COLOR_STOP_BG lv_color_hex(0x900000)

// Button colors.
#define COLOR_BTN_BG lv_color_hex(0xffbaf8)
#define COLOR_PAUSE_BG lv_color_hex(0xbaffc1)
#define COLOR_RESUME_BG lv_color_hex(0xbaffe3)
#define COLOR_CANCEL_BG lv_color_hex(0x900000)
// Shown while a cancel is armed and waiting for its second tap.
#define COLOR_CONFIRM_BG lv_color_hex(0xE04A32)
#define COLOR_HOMED_BG lv_color_hex(0xffe3ba)

// Corner symbols on the tool page. These are inks on a dark disc rather than
// fills, so they are the one place a saturated colour is safe.
#define COLOR_LOAD lv_color_hex(0xbaffc1)
#define COLOR_UNLOAD lv_color_hex(0xffe3ba)

// G-code screen colors.
#define COLOR_GCODE_UNSELECTED lv_color_hex(0x808080)
#define COLOR_GCODE_HIGHLIGHT lv_color_hex(0x444444)

#define SERIAL_BAUD_RATE 115200
#define SERIAL_TIMEOUT 1000

#endif
