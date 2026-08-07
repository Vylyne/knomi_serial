#ifndef PRINTER_H
#define PRINTER_H

namespace printer {

// Wire format version for the host -> device State packet below. Bump this
// whenever the layout of State changes so a mismatched host/firmware pair
// reports itself instead of silently rendering garbage. Must be kept in sync
// with _PROTO_VERSION in klippy_extras/knomi_serial.py.
static const unsigned int kProtoVersion = 2;

//: Longest filament type name carried in State, excluding its terminator.
static const unsigned int kFilamentTypeMaxLen = 15;

enum class Status {
  kDisconnected = 0x00,
  kIdle         = 0x01,
  kPrinting     = 0x02,
  kShutdown     = 0x03
};

enum class TramType {
  kNone = 0x00,
  kZTA  = 0x01,
  kQGL  = 0x02
};

// Field order and padding here are the wire format - klippy_extras builds this
// byte by byte in _send_state. The three-byte gap after the flags keeps the
// int32 block 4-byte aligned; adding a flag consumes padding rather than
// shifting everything after it.
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

  char _padding[1];

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
  int32_t tool_number = -1;

  //: 0x00RRGGBB, as sliced. Zero means unknown, not black.
  uint32_t filament_color = 0;

  TramType tram_type = TramType::kNone;

  char filament_type[kFilamentTypeMaxLen + 1];

  char gcodes[256];
};

// The wire format is this struct's memory layout - recv_task memcpys straight
// into it - so a compiler that pads differently than the host packs would
// corrupt every field after the change, silently. Pinning the size turns that
// into a build failure. If this fires, the layout moved: match _STATE_FMT in
// klippy_extras/knomi_serial.py and bump kProtoVersion above.
static_assert(sizeof(State) == 332, "State layout changed: update _STATE_FMT and kProtoVersion");

}

#endif