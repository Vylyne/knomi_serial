#!/usr/bin/env python3
"""Pull a picture of the glass over the serial link.

The display has no network, no filesystem to dump to and no second port, so the
frame comes back the way everything else does: as lines of text on the link that
is already open. About fourteen seconds for 240x240.

    python scripts/screenshot.py COM5 -o docs/img/printing.png

Nothing else may be driving the port at the time - stop Klipper, or use
--drive to have this script feed the display a state of its own first:

    python scripts/screenshot.py COM5 --drive printing -o docs/img/printing.png

--all regenerates every image the README uses, including the lost-link one that
needs the script to stop talking and wait out the device's watchdog. Run it
after any visual change:

    python scripts/screenshot.py COM5 --all

The PNG is written with the corners outside the round bezel made transparent,
because the panel holds pixels there that nobody can see.
"""

import argparse
import base64
import math
import os
import struct
import sys
import time
import zlib

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(_HERE), "klippy_extras"))

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed. pip install pyserial")

import knomi_serial as k

#: Long enough for the whole frame at 115200 baud, with room for a display that
#: is busy when the request lands.
_TIMEOUT = 40.0

PRESETS = {
    "idle": dict(status=k.PrinterStatus.IDLE),
    "printing": dict(status=k.PrinterStatus.PRINTING, working=True, progress=54,
                     flow=2600),
    "heating": dict(status=k.PrinterStatus.IDLE, hotend_temp=160, working=True),
    "shutdown": dict(status=k.PrinterStatus.SHUTDOWN),
    "waiting": dict(status=k.PrinterStatus.DISCONNECTED),
}

#: What the shutdown screen has to say. Klipper sends this on its own frame, so
#: a preset that only sets the status would photograph whatever message the
#: display happened to be holding.
MESSAGES = {
    "shutdown": "MCU 'MCU' SHUTDOWN: LOST COMMUNICATION",
    "waiting": "MCU 'MCU' SHUTDOWN: LOST COMMUNICATION",
}


def state_for(preset, config_crc, color=0x9572BF, ftype=b"ABS"):
    # Deliberately not the machine's own pink. The two colours mean different
    # things - one is the printer, one is what is loaded in it - and a
    # documentation shot that uses the same value for both cannot show that.
    base = dict(
        status=k.PrinterStatus.IDLE,
        homed_x=True, homed_y=True, homed_z=True,
        used=True, active=True, tool_number=0,
        hotend_temp=243, hotend_target=245,
        bed_temp=100, bed_target=100,
        chamber_temp=50, chamber_target=50, mcu_temp=42,
        filament_color=color, filament_type=ftype,
        config_crc=config_crc,
    )
    base.update(PRESETS[preset])
    return k.encode_state(k.PrinterState(**base))


def write_png(path, width, height, pixels, mask_round=True):
    """pixels is a list of (r, g, b). Written RGBA, no dependencies."""
    radius = width / 2.0
    cx = cy = radius - 0.5

    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter type 0
        for x in range(width):
            r, g, b = pixels[y * width + x]
            a = 255
            if mask_round and math.hypot(x - cx, y - cy) > radius:
                # The panel is round. These pixels exist in the framebuffer and
                # on no part of the display anyone can see.
                a = 0
            rows += bytes((r, g, b, a))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    # 8 bits per channel, colour type 6 (RGBA).
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(png)


def rgb565_to_rgb888(raw, count):
    """Native-endian RGB565 off the wire to 8-bit triples.

    The low bits are replicated into the empty ones rather than left at zero, so
    full white comes back as 255 rather than 248.
    """
    out = []
    for i in range(count):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out.append(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return out


def capture(port, drive_frame, config, config_crc, verbose):
    port.write(k.encode_frame(0x04, b""))

    buf = b""
    payload = bytearray()
    size = None
    started = time.time()
    last_note = 0.0

    while time.time() - started < _TIMEOUT:
        if drive_frame is not None and time.time() - last_note > 0.1:
            # Keep feeding state, or the display's own staleness watchdog raises
            # the disconnected mark in the middle of the shot.
            port.write(drive_frame)
            last_note = time.time()

        if not port.in_waiting:
            time.sleep(0.01)
            continue

        buf += port.read(port.in_waiting)
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            line = line.strip()
            if line == b"KNOMI_CMD:CFG?":
                port.write(k.encode_config(config))
                continue
            if not line.startswith(b"KNOMI_CMD:SNAP:"):
                continue
            body = line[len(b"KNOMI_CMD:SNAP:"):]

            if body.startswith(b"BEGIN:"):
                spec = body[6:].decode("ascii", "replace").split(",")
                size = (int(spec[0]), int(spec[1]))
                payload = bytearray()
                if verbose:
                    print(f"  receiving {spec[0]}x{spec[1]} {spec[2]}")
                continue
            if body.startswith(b"ERR:"):
                raise SystemExit(
                    "  device refused: " + body[4:].decode("ascii", "replace"))
            if body == b"END":
                if size is None:
                    raise SystemExit("  END with no BEGIN")
                return size, bytes(payload)
            if size is not None:
                payload += base64.b64decode(body)
                want = size[0] * size[1] * 2
                # Every twentieth line. On a terminal the carriage return makes
                # this one updating counter; piped to a file it should not be
                # three hundred of them.
                if verbose and len(payload) % (384 * 20) == 0:
                    print(f"\r  {len(payload):>6}/{want} bytes", end="", flush=True)

    raise SystemExit("  timed out waiting for the frame")


def shoot(port, out, preset, config, config_crc, args, quiet_first=False):
    """One capture, start to PNG."""
    verbose = not args.quiet
    frame = None

    if preset:
        try:
            color = int(args.color.strip().lstrip("#"), 16)
        except ValueError:
            sys.exit(f"  --color '{args.color}' is not hex")
        frame = state_for(preset, config_crc, color, args.type.encode("utf-8")[:15])
        if verbose:
            print(f"  {os.path.basename(out):<16} driving '{preset}'")

        # The device may have just been reset by opening the port, so give it
        # long enough to boot, ask for config and load its screen.
        deadline = time.time() + max(args.settle, 3.0)
        buf = b""
        if preset in MESSAGES:
            port.write(k.encode_message(MESSAGES[preset]))
        while time.time() < deadline:
            port.write(frame)
            if port.in_waiting:
                buf += port.read(port.in_waiting)
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    if line.strip() == b"KNOMI_CMD:CFG?":
                        port.write(k.encode_config(config))
            time.sleep(0.1)

    if quiet_first:
        # Stop feeding it and wait out the device's staleness watchdog, so the
        # shot shows the lost-link mark. Capturing must then not drive either,
        # or the first frame would clear what we came to photograph.
        if verbose:
            print(f"  {'':<16} going quiet for {args.quiet_for:.0f}s")
        time.sleep(args.quiet_for)
        frame = None

    size, raw = capture(port, frame, config, config_crc, verbose)
    width, height = size
    want = width * height * 2
    if len(raw) != want:
        sys.exit(f"  short frame: {len(raw)} of {want} bytes")

    pixels = rgb565_to_rgb888(raw, width * height)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    write_png(out, width, height, pixels, mask_round=not args.square)
    if verbose:
        print(f"  {'':<16} wrote {out}")


#: The documentation set: (file stem, preset, go quiet first).
#:
#: Here rather than in a shell loop because regenerating these is a thing that
#: happens after every visual change, and reassembling the commands by hand each
#: time is how the stale shot ended up being taken three different ways.
DOC_SHOTS = [
    ("waiting", "waiting", False),
    ("heating", "heating", False),
    ("idle", "idle", False),
    ("printing", "printing", False),
    ("shutdown", "shutdown", False),
    ("stale", "idle", True),
]


def main():
    p = argparse.ArgumentParser(
        description="Capture a Knomi_Serial display over the serial link.")
    p.add_argument("port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    p.add_argument("-o", "--out", default="screenshot.png")
    p.add_argument("--all", action="store_true",
                   help="regenerate the whole documentation set into --dir")
    p.add_argument("--dir", default=os.path.join("docs", "img"),
                   help="where --all writes (default docs/img)")
    p.add_argument("--quiet-for", type=float, default=5.0,
                   help="seconds of silence for the stale shot, which must "
                        "exceed the firmware's STALE_TIMEOUT_MS")
    p.add_argument("--drive", choices=sorted(PRESETS),
                   help="feed the display this state first, instead of "
                        "photographing whatever a running Klipper is showing")
    p.add_argument("--color", "--colour", dest="color", default="9572BF",
                   help="filament colour for --drive, RRGGBB (default 9572BF, "
                        "chosen to differ from the machine accent)")
    p.add_argument("--type", default="ABS", help="filament type for --drive")
    p.add_argument("--settle", type=float, default=1.5,
                   help="seconds to let the screen settle before capturing")
    p.add_argument("--square", action="store_true",
                   help="keep the corners the round bezel hides")
    p.add_argument("-q", "--quiet", action="store_true")
    args = p.parse_args()

    # The accent is set explicitly rather than left to the firmware default,
    # because the device now remembers the last config it was given - so a shot
    # taken after somebody's experiment would quietly inherit their colour.
    config = k.DeviceConfig(
        present=k._HAS_COLOR_MACHINE | k._HAS_GCODES,
        color_machine=0xFFA7C4,
        gcodes=b"HOME\nQGL\nPURGE\nCLEAN_NOZZLE")
    config_crc = zlib.crc32(k.config_payload(config))

    try:
        port = serial.Serial(args.port, k._BAUD_RATE, write_timeout=2.0)
    except serial.SerialException as e:
        sys.exit(f"Could not open {args.port}: {e}")

    try:
        if args.all:
            for stem, preset, quiet_first in DOC_SHOTS:
                out = os.path.abspath(os.path.join(args.dir, stem + ".png"))
                shoot(port, out, preset, config, config_crc, args, quiet_first)
        else:
            shoot(port, os.path.abspath(args.out), args.drive,
                  config, config_crc, args)
    finally:
        try:
            port.write(k.encode_state(
                k.PrinterState(status=k.PrinterStatus.DISCONNECTED)))
            port.flush()
            port.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
