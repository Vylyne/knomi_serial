#ifndef PRINTER_H
#define PRINTER_H

#include <stddef.h>
#include <stdint.h>

namespace printer {

// Wire format version. Bump this whenever any frame layout below changes, so a
// mismatched host/firmware pair reports itself instead of silently rendering
// garbage. Must be kept in sync with _PROTO_VERSION in
// klippy_extras/knomi_serial.py.
//
// 3: split the one packet into typed frames. Everything that does not change
//    from tick to tick left the state packet, which is most of what was in it -
//    the macro list alone was 256 of its 332 bytes and was resent ten times a
//    second for the life of the machine. See kFrame below.
// 4: config says which pages a screen has and in what order, so there is one
//    firmware instead of a toolchanger build and a non-toolchanger one.
static const unsigned int kProtoVersion = 4;

// Every frame is
//
//     HEADER(4)  TYPE(1)  LEN(2)  PAYLOAD(LEN)  FOOTER(4)
//
// with LEN and every multi-byte field in network order.
//
// Proto 2 had one implicit frame type and inferred the length from the struct,
// which is why anything the screen needed had to be in the tick or nowhere. A
// type byte costs one byte per frame and buys a channel for everything that is
// true for hours at a time.
enum class Frame : uint8_t {
  //: The machine right now, ten times a second.
  kState = 0x01,
  //: Settings. Sent when the device asks, which it does when the config CRC in
  //: the state frame stops matching what it is holding.
  kConfig = 0x02,
  //: Something to show the operator - currently the reason Klipper shut down.
  kMessage = 0x03,
  //: Send back a picture of the glass. Carries no payload; see
  //: scripts/screenshot.py. A developer and documentation tool - it holds the
  //: serial link for about fourteen seconds.
  kSnapshot = 0x04,
};

static const unsigned int kFilamentTypeMaxLen = 15;
static const unsigned int kGcodesMaxLen = 255;
static const unsigned int kMessageMaxLen = 127;

enum class Status : uint32_t {
  kDisconnected = 0x00,
  kIdle         = 0x01,
  kPrinting     = 0x02,
  kShutdown     = 0x03
};

//: One byte now rather than four. It has three values and sat in the middle of
//: the int32 block spending a word on them.
enum class TramType : uint8_t {
  kNone = 0x00,
  kZTA  = 0x01,
  kQGL  = 0x02
};

//: Sent when the host does not know - a print with no slicer estimate, or a
//: layer count nothing has reported. Distinct from zero, which is a real answer.
static const int32_t kUnknown = -1;

// Everything down to `filament_type` is the kState payload, in wire order.
// Fields after it arrive on other frames and are held here so the UI has one
// place to read the machine from.
//
// Field order and padding are the wire format. The seven flags and the
// one-byte tram type fill the gap after `status` exactly, which is what keeps
// the int32 block 4-byte aligned; adding a flag would consume that padding
// rather than shifting everything below it.
struct State {
  Status status = Status::kDisconnected;

  bool working = false;
  bool paused = false;

  bool homed_x = false;
  bool homed_y = false;
  bool homed_z = false;

  //: This tool is part of the running job. The host decides this - it is told
  //: which tools a job uses rather than inferring it from temperature.
  bool used = true;
  //: This tool is the one currently mounted.
  bool active = false;

  TramType tram_type = TramType::kNone;

  int32_t hotend_temp   = 0;
  int32_t hotend_target = 0;

  int32_t bed_temp   = 0;
  int32_t bed_target = 0;

  int32_t chamber_temp   = 0;
  int32_t chamber_target = 0;

  int32_t mcu_temp = 0;
  int32_t mcu_target = 0;

  int32_t progress = 0;

  //: -1 when the section declares no `tool:`.
  int32_t tool_number = kUnknown;

  //: 0x00RRGGBB, as sliced. Zero means unknown, not black.
  uint32_t filament_color = 0;

  //: Extrusion rate in micrometres of filament per second, signed - negative
  //: is a retraction.
  //:
  //: A rate rather than the extruder position, and a rate rather than the
  //: change since the last packet. Position aliases badly at 10Hz: a fast
  //: extrude and a slow one can land on the same pair of samples, so motion
  //: derived from it would lie about speed. A per-packet delta fixes that but
  //: still divides by an interval the device only knows approximately, while
  //: the host knows exactly when it sampled. So the host does the division.
  int32_t flow = 0;

  //: Seconds left and seconds so far, or kUnknown. Progress alone cannot say
  //: "twenty minutes", which is the thing anyone walking past wants to know.
  int32_t eta = kUnknown;
  int32_t elapsed = kUnknown;

  //: Current and total layer, or kUnknown when the slicer did not say.
  int32_t layer = kUnknown;
  int32_t layer_total = kUnknown;

  //: CRC32 of the config payload the host is holding.
  //:
  //: This is the whole config synchronisation mechanism. The host stamps it on
  //: every state frame at no cost - it is precomputed and never recalculated
  //: per tick - and the device compares it against the CRC of the config it
  //: actually has. Any disagreement, for any reason, and the device asks for
  //: config again: a device reset, a Klipper restart, a config edit, a cable
  //: pulled mid-frame, or a frame that arrived corrupt. There is no acking, no
  //: retry timer and no host-side per-device bookkeeping, because a content
  //: hash cannot get out of step with the content the way a sequence number can.
  uint32_t config_crc = 0;

  char filament_type[kFilamentTypeMaxLen + 1] = {0};

  // ---- end of the kState payload ----

  //: Last kMessage. The reason Klipper shut down, or a local fault.
  char message[kMessageMaxLen + 1] = {0};
};

//: Length of the kState payload. Everything in State from `message` on arrives
//: some other way and must not be read off a state frame.
static const unsigned int kStateWireSize = 96;

// The wire format is this struct's memory layout - recv_task memcpys straight
// into it - so a compiler that padded differently than the host packs would
// corrupt every field after the change, silently. Pinning both the payload size
// and where the payload stops turns that into a build failure.
//
// If either fires, the layout moved: match _STATE_FMT in
// klippy_extras/knomi_serial.py and bump kProtoVersion above.
static_assert(
    offsetof(State, message) == kStateWireSize,
    "State wire layout changed: update _STATE_FMT and kProtoVersion");
static_assert(
    sizeof(((State *)0)->filament_type) == kFilamentTypeMaxLen + 1,
    "filament_type is a fixed-width wire field");

// Settings pushed from printer.cfg, so they survive a reflash and are edited
// where the user already edits the machine.
//
// What belongs here is anything the host knows and the device cannot, plus
// anything that is taste. The compile-time values in user_conf.h are the
// defaults these start at, so a printer.cfg that says nothing behaves exactly
// as the firmware was built to.
//: Which fields of a Config the host actually set. Anything unset keeps the
//: value the firmware was built with.
//:
//: Without this, a host that fills in its own defaults quietly becomes the
//: authority on every setting - and editing user_conf.h and reflashing would
//: appear to work until the first config frame arrived and put it all back.
//: "Override" has to mean override, not replace.
enum ConfigHas : uint32_t {
  kHasColorMachine   = 1u << 0,
  kHasColorUnknown   = 1u << 1,
  kHasDimMs          = 1u << 2,
  kHasSleepMs        = 1u << 3,
  kHasBrightness     = 1u << 4,
  kHasDimBrightness  = 1u << 5,
  kHasGcodes         = 1u << 6,
  kHasKeyMask        = 1u << 7,
  kHasPageOrder      = 1u << 8,
  kHasEstopAt        = 1u << 9,
};

//: Which side of the page row the emergency stop hangs off.
//:
//: Below means you drag upward to reach it. Above is the notification-shade
//: gesture - drag down. Which of those is "obvious" is not a fact about the
//: firmware, so it is not the firmware's to decide.
enum class EstopAt : uint8_t {
  kBottom = 0,
  kTop = 1,
};

//: The pages an idle screen can carry, as they appear on the wire.
//:
//: There used to be a TOOLCHANGER build flag that compiled two of these out.
//: It saved 1572 bytes of a 4.7MB flash - and cost a second firmware to build,
//: test, publish and pick between, where flashing the wrong one silently
//: removed pages with nothing on screen to say why. Since off-screen pages are
//: neither updated nor drawn, the real cost of an unwanted page is one more
//: swipe to get past it, which is a preference and belongs in printer.cfg.
enum class Page : uint8_t {
  //: Terminates page_order. Never a page.
  kNone  = 0,
  kTool  = 1,
  kGcode = 2,
  kHome  = 3,
  kMove  = 4,
  //: Always last and never listed - it is appended whatever the order says.
  //: It was inside the old build flag, so toolchanger builds had no emergency
  //: stop at all, and that is not a mistake worth leaving available.
  kEstop = 5,
};

//: Longest page_order, terminator included.
static const unsigned int kMaxPages = 8;

//: Bits of Config.key_mask - the corners that are legends rather than soft
//: keys, because something else already reports the press.
enum KeySlot : uint8_t {
  kKeyNW = 1u << 0,
  kKeyNE = 1u << 1,
  kKeySW = 1u << 2,
  kKeySE = 1u << 3,
};

struct Config {
  //: Bitmask of ConfigHas. Only meaningful on the wire - once applied, every
  //: field holds either the host's value or the compiled-in default.
  uint32_t present;

  //: The printer's own accent. Not everyone likes pink.
  uint32_t color_machine;
  //: Shown when the host has not said what is loaded.
  uint32_t color_filament_unknown;

  //: Idle milliseconds before the backlight drops, and before it goes out.
  uint32_t dim_ms;
  uint32_t sleep_ms;

  uint8_t brightness;
  uint8_t dim_brightness;

  //: Corners that are legends rather than touch targets, as KeySlot bits. A
  //: corner with a switch behind it - wired to the device, or bound to a
  //: [gcode_button] on the host - keeps its symbol and loses its hit region,
  //: because two ways to fire one action, one of them invisible, is a way to
  //: fire it by accident.
  uint8_t key_mask;

  //: An EstopAt. This was the struct's one padding byte, so giving it a meaning
  //: changed no offsets and needed no version bump - a host that predates it
  //: sends zero, which is the behaviour that byte already had. That is the
  //: presence-bit design paying off: `present` says what was actually set, so a
  //: reserved byte can be claimed without the two ends disagreeing about size.
  uint8_t estop_at;

  //: Which pages the idle screen carries, in order, terminated by kNone. The
  //: screen lands on the first of them, so ordering chooses both the sequence
  //: and where you start.
  //:
  //: A page with nothing in it is skipped whatever this says - listing `gcode`
  //: with no macros configured gets you no G-code page, because a page that
  //: can only be empty is worse than one that is absent.
  uint8_t page_order[kMaxPages];

  //: Newline-separated macro names for the gcode page. 256 bytes, and the
  //: reason proto 2 spent three quarters of its bandwidth restating a list
  //: that had not changed since Klipper started.
  char gcodes[kGcodesMaxLen + 1];
};

static const unsigned int kConfigWireSize = 288;
static_assert(
    sizeof(Config) == kConfigWireSize,
    "Config layout changed: update _CONFIG_FMT and kProtoVersion");

//: Largest payload any frame carries, for sizing the receive buffer.
static const unsigned int kMaxPayload = kConfigWireSize;

}

#endif
