# Knomi V2 hardware notes

Read off the [BTT Knomi V2.0 schematic](https://github.com/bigtreetech/KNOMI/blob/master/KNOMI2/Hardware/BIGTREETECH%20Knomi%20V2.0-SCH.pdf) (rev V2.2, 23 Oct 2023). Every net listed as *in use* below cross-checks against `src/knomi_v2.h`, GPIO12 is the backlight, 14/18/20/19/21 are the LCD, 16/17 are the touch panel.

## The parts that matter

| Ref | Part | Notes |
| --- | --- | --- |
| U1 | ESP32-S3**R8** | The `R8` is 8 MB in-package PSRAM. Nearly all idle — see below. |
| U3 | LH128R-IC15-TP | 240×240 round GC9A01 panel with capacitive touch. |
| U5 | CH340K | USB-to-UART, enumerating as `1a86:7522`. The only serial path — see below. |
| U6 | MX1.25 4-pin | External I²C port. Pullups fitted. |
| U7 | GD25Q128E | 128 Mbit = 16 MB flash. |
| U10 | FPC 24-pin 0.5 mm | Camera connector, labelled `ov2640`. Unpopulated. |
| U13 | AW9364 | Backlight LED driver. |
| P2 | MX1.25 2-pin | External power input, labelled `batter`. |

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

## Connectors

Pin numbers are the schematic's. The schematic does not show which physical end
of a connector is pin 1, so confirm it with a meter before wiring — all three
have a ground pin that is easy to find by continuity.

### U6 — external I²C (MX1.25, 4-pin)

| Pin | Net | Notes |
| --- | --- | --- |
| 1 | `VDD33` | 3.3 V out. C34 decouples it at the connector. |
| 2 | `GND` | — |
| 3 | `SCL0` | GPIO1. |
| 4 | `SDA0` | GPIO2. |
| 5, 6 | `GND` | Mounting tabs. |

Both lines are pulled to 3.3 V twice: 4.7K drawn with the connector (R23/R24)
and 10K drawn with the accelerometer (R44/R45), about 3.2K in parallel if both
pairs are fitted. A
peripheral that brings its own pullups lowers that further.

### U10 — camera FPC (24-pin, 0.5 mm)

The standard OV2640 module pinout. *Label* is the connector's name for the pin;
*net* is what the board wires it to.

| Pin | Label | Net | GPIO | Notes |
| --- | --- | --- | --- | --- |
| 1 | `NC` | — | — | Not connected. |
| 2 | `GND` | `GND` | — | — |
| 3 | `SDA` | `SDA1` | 4 | R31, 2K to `VDD28`. |
| 4 | `AVDD` | `AVDD28` | — | `VDD28` through R59 (0R). |
| 5 | `SCL` | `SCL1` | 3 | R30, 2K to `VDD28`. Strapping pin — see above. |
| 6 | `RESET` | `RESET1` | 11 | — |
| 7 | `VS` | `VSYNC` | 15 | — |
| 8 | `PWDN` | `PWDN` | 10 | R55, 100K to GND. |
| 9 | `HS` | `HREF` | 9 | R56, 20R in series. |
| 10 | `DVDD12` | `VDD12` | — | U12's output. |
| 11 | `DOVDD28` | `VDD28` | — | U11's output, 2.8 V. |
| 12 | `D9` | `D9` | 8 | — |
| 13 | `MCLK` | `MCLK` | 7 | R57, 20R in series. |
| 14 | `D8` | `D8` | 6 | — |
| 15 | `GND1` | `GND` | — | — |
| 16 | `D7` | `D7` | 5 | — |
| 17 | `PCLK` | `PCLK` | 48 | R58, 20R in series. |
| 18 | `D6` | `D6` | 47 | — |
| 19 | `D2` | `D2` | 38 | — |
| 20 | `D5` | `D5` | 39 | — |
| 21 | `D3` | `D3` | 40 | — |
| 22 | `D4` | `D4` | 41 | — |
| 23 | `D1` | `D1` | 42 | — |
| 24 | `D0` | `D0` | 45 | Strapping pin — see above. |
| 25, 26 | — | `GND` | — | Shell. |

The schematic symbol draws odd pins on one side and even on the other, but the
ribbon is a single row. The four preferred button pins are not adjacent on it:
`HREF` is 9, `D9` is 12, `D8` is 14 and `D7` is 16. Ground is on 2 and 15.

`VDD12` is named for 1.2 V but U12 is marked PW6566B**15**, which suggests
1.5 V. Meter it before powering anything from pin 10.

### P2 — power input (MX1.25, 2-pin)

| Pin | Net | Notes |
| --- | --- | --- |
| 1 | `VIN` | 2.5–5.5 V, per the schematic. **Not a 12 V or 24 V input.** |
| 2 | `GND` | — |
| 3, 4 | `GND` | Mounting tabs. |

`VIN` and USB `VBUS` each pass through a Schottky (V1 and V2, DSS24) into the
same node, then a 2 A fuse and the PW2052 buck that makes `VDD33`. The diodes
mean either source can power the board and neither back-feeds the other — the
display can be fed from P2 while USB stays plugged in for serial.

## The 8 MB of PSRAM

It is initialised — `BOARD_HAS_PSRAM` is in the board definition, and the
device reports 8.0 MB free in every status line. One thing uses it: the
screenshot framebuffer, 115,200 bytes taken with `ps_malloc` when a capture is
asked for and freed when it is sent. Nothing holds any of it between captures.

**Leaving it idle is the right answer, not an oversight.** The obvious use would
be full-screen LVGL draw buffers instead of the two 11.5 KB statics in internal
DRAM, and that would make things slower. Rendering is memory-write heavy — fills
and blends — and octal PSRAM manages tens of MB/s against internal SRAM's
hundreds. The SPI transfer is the same number of pixels either way, so the only
gain is a few fewer flush calls, bought at the cost of every pixel being written
across a slower bus.

What PSRAM is good for here is exactly what it is doing: something large,
short-lived, and written once. A screenshot qualifies. A framebuffer redrawn
thirty times a second does not.

If the UI ever does outgrow its budget, the lever is DMA rather than memory.
`_flush_display` currently calls `pushColors` and then reports the flush
complete immediately, which is synchronous — so LVGL cannot render the next
strip while the last one transfers, and the second draw buffer it was given
earns nothing. TFT_eSPI can push over DMA on the ESP32, which would overlap the
two. It is not needed today: an idle screen costs 0.2% of the UI task and the
printing screen with its wave running costs about 17%.

## Three consequences

**Native USB is physically impossible on this board.** GPIO19 and GPIO20 are the
ESP32-S3's USB D− and D+, and the panel uses both for D/C and chip select. That
is why there is a CH340K, why `boards/knomi.json` turns `wait_for_upload_port`
off — the CH340 never leaves the bus, so no new port appears after a reset —
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

The keys need to reach two boards at once — the buffer already acts on feed and
retract without Klipper's involvement, and that channel is worth keeping. A DPDT
momentary switch does that with two galvanically separate poles and no active
parts, which sidesteps the question of whether both inputs share a logic rail.
Worth metering before wiring: plenty of 6-pin 7×7 tactiles are two poles that
are internally common, which would tie the two boards' grounds together and
defeat the point.

For the Knomi's side of that switch, **direct GPIO through U10 beats an I²C
expander** now that the FPC is broken out. Four pins, nothing shared with the
touch panel, and a firmware poll every 5 ms with 20 ms stable-state debounce.
An expander on U6
remains a reasonable fallback purely on mechanical grounds — a crimped JST
connector will not walk out of its latch the way a 0.5 mm ribbon can — but it
polls, and its interrupt line has nowhere to land on a four-pin connector.

Good pins for buttons, all plain GPIO with no strapping or boot meaning and
usable internal pull-ups:

| GPIO | FPC net |
| --- | --- |
| 5 | `D7` |
| 6 | `D8` |
| 8 | `D9` |
| 9 | `HREF` |

The first implementation configures only GPIO bindings named in `printer.cfg`
as `INPUT_PULLUP`. Wire each switch from its GPIO to Knomi ground: open reads
HIGH, closed reads LOW. The supported Knomi V2 set is GPIO5, GPIO6, GPIO8,
GPIO9, GPIO11, GPIO15, GPIO38–GPIO42, GPIO47, and GPIO48. The four pins above
are preferred because their FPC nets are easy to identify. An old binding's
pull-up is removed when the config changes. This has compiled successfully;
live switch behavior still needs verification on a wired board. The live
acceptance checks for debounce, config replacement, and touch suppression are
in [verification.md](verification.md#live-acceptance-checks); compilation alone does not
complete them.

Avoid **GPIO10** for a pull-up button input. R55 ties it to ground through 100K,
and against the S3's ~45K internal pull-up that divides to roughly 2.28 V —
under the ~2.48 V V<sub>IH</sub>, so a released button would read as pressed. It
is fine with an external pull-up of a few kΩ, which swamps R55.

And not **GPIO45**, which is the `VDD_SPI` strapping pin: held wrong during
reset, the flash rail comes up at the wrong voltage and the board does not boot.

## Checking the bus

`pio run -e knomi_i2cscan -t upload` builds with a boot-time I²C scan that
reports over the same serial link, so `scripts/simulate.py` prints the result.
It answers what the schematic can only imply — whether U6 really shares the
touch panel's bus, and whether that LIS2DW12 footprint is populated. `0x15`
appearing means the touch controller is on the bus being scanned.

It runs before any task starts, so nothing else is using Wire at the time. Reset
to scan again; there is no background polling, because sharing the bus with the
touch driver from a second task is a race not worth adding to a diagnostic.
