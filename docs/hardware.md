# Knomi V2 hardware notes

Read off the BTT Knomi V2.0 schematic (rev V2.2, 23 Oct 2023). Every net listed
as *in use* below cross-checks against `src/board_conf.h`, so the mapping is
confirmed rather than inferred — GPIO12 is the backlight, 14/18/20/19/21 are the
LCD, 16/17 are the touch panel, and those are exactly the numbers the firmware
already drives.

## The parts that matter

| Ref | Part | Notes |
| --- | --- | --- |
| U1 | ESP32-S3**R8** | The `R8` is 8 MB in-package PSRAM. Currently unused. |
| U3 | LH128R-IC15-TP | 240×240 round GC9A01 panel with capacitive touch. |
| U5 | CH340K | USB-to-UART. The only serial path — see below. |
| U6 | MX1.25 4-pin | External I²C port. Pullups fitted. |
| U7 | GD25Q128E | 128 Mbit = 16 MB flash. |
| U10 | FPC 24-pin 0.5 mm | Camera connector, labelled `ov2640`. Unpopulated. |
| U13 | AW9364 | Backlight LED driver. |

## Pin map

### In use

| GPIO | Net | Purpose |
| --- | --- | --- |
| 0 | `BOOT` | Boot strapping, tied to SW-4. |
| 1 | `SCL0` | I²C bus 0 clock — touch panel **and** U6. |
| 2 | `SDA0` | I²C bus 0 data — touch panel **and** U6. |
| 12 | `Backlight` | AW9364 enable. PWM dimming. |
| 13 | `LCD-SDO` | Panel MISO. Wired, but `tft_setup.h` never declares it. |
| 14 | `LCD-SDA` | Panel MOSI. |
| 16 | `TP_RST` | Touch reset. |
| 17 | `TP_INT` | Touch interrupt. |
| 18 | `LCD-CLK` | Panel SCLK, run at 80 MHz. |
| 19 | `WRX/RS` | Panel D/C. **Also the ESP32-S3's USB D−.** |
| 20 | `LCD-CS` | Panel chip select. **Also the ESP32-S3's USB D+.** |
| 21 | `Screen-rst` | Panel reset. |
| 43 / 44 | `U0TXD` / `U0RXD` | To the CH340K. Do not repurpose. |

### Free, but only reachable through U10

The camera bus. Nothing in the firmware touches any of it.

| GPIO | Net | Caveat |
| --- | --- | --- |
| 3 | `SCL1` | Strapping pin (JTAG source select). |
| 4 | `SDA1` | — |
| 5 | `D7` | — |
| 6 | `D8` | — |
| 7 | `MCLK` | — |
| 8 | `D9` | — |
| 9 | `HREF` | — |
| 10 | `PWDN` | R55 pulls it to GND through 100K. |
| 11 | `RESET1` | — |
| 15 | `VSYNC` | Also `XTAL_32K_P`. |
| 38 | `D2` | — |
| 39–42 | `D5`, `D3`, `D4`, `D1` | The JTAG pins. Usable; costs hardware debug. |
| 45 | `D0` | **Strapping pin — selects VDD_SPI voltage.** Pull it at boot and the flash rail changes. Treat as output-after-boot only, or leave it. |
| 47 / 48 | `D6`, `PCLK` | — |

Eighteen signals, but at 0.5 mm pitch they need a breakout before anything can
be soldered, and the connector's I/O rail is `VDD28` — the `SCL1`/`SDA1` pullups
(R30/R31, 2K) go to 2.8 V, not 3.3 V. The S3 reads 2.8 V as high, its V<sub>IH</sub>
being about 2.48 V, but that is a 320 mV margin rather than a comfortable one.

### Not available

`GPIO33`–`GPIO37` are marked no-connect on the schematic because the S3**R8**'s
octal PSRAM consumes them internally. They are not free pins; they are spoken for
inside the package.

## Three consequences

**Native USB is physically impossible on this board.** GPIO19 and GPIO20 are the
ESP32-S3's USB D− and D+, and the panel uses both for D/C and chip select. That
is why there is a CH340K, why `board_upload.wait_for_upload_port = no` is
required — the CH340 never leaves the bus, so no new port appears after a reset —
and why USB serial was removed from this firmware earlier. It was never a
configuration mistake; the pins are gone.

**U6 shares its bus with the touchscreen.** The external port is `SCL0`/`SDA0` on
GPIO1/GPIO2, which is the same bus the CST816S answers on at `0x15`. Anything
plugged into U6 joins that bus. In practice this is convenient — `Wire` is
already running, so a peripheral needs an address and nothing else — but a device
that jams the bus takes the touchscreen down with it.

Address space is otherwise clear: touch at `0x15`, a PCF8574 or MCP23017 would
sit at `0x20`–`0x27`, and an LIS2DW12 would be `0x18`/`0x19`. Scanning the bus is
also the cheapest way to find out whether that accelerometer is actually
populated on your board.

**There is a second, entirely unused I²C bus.** `SCL1`/`SDA1` on GPIO3/GPIO4 is
the camera's SCCB bus and the firmware never initialises it. A peripheral there
would be isolated from the touch panel — the failure mode above disappears — at
the cost of getting at the FPC, and of the 2.8 V pullup rail.

## For the four corner keys

Two routes, and they differ in latency rather than capability:

- **Buffer board GPIO, as `[gcode_button]`.** No firmware work at all; Klipper
  handles debounce and dispatch. But a press travels button → MCU → host → macro
  → packet → screen, so the screen cannot react in under ~100–200 ms.
- **An I²C expander on U6.** Needs polling and debounce in firmware, and shares
  the touch bus. In exchange the screen can respond on the same frame while the
  gcode still goes up through Klipper.

The second only matters because of what the keys are meant to do: the fill
leaning toward the key you pressed is feedback, and feedback that arrives 200 ms
late stops reading as caused by you. Feedback local, authority central.
