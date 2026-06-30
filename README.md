# Knomi_Serial

### Acknowledgement

This is a fork of [ruiqimao/zerod](https://github.com/ruiqimao/zerod). I forked this update so I could compile it and I'm tweaking it for my own personal use with a toolchanger

## Round display for klipper printers

Knomi_Serial is an alternative firmware for the BTT Knomi V2 and other similar displays that replaces the
network-reliant Moonraker connection with a direct serial connection to the Klipper host.

### Installation

Knomi_Serial firmware can be built and flahsed using [PlatformIO](https://platformio.org/).

An additional Klipper module is needed to enable the Knomi_Serial configuration options, which can be
installed by running the included `install.sh` script on the Klipper host.

### Klipper configuration
```ini
[knomi_serial T0_knomi] # both a named device like this or a single like [knomi_serial] are available.
serial:  # Path to the serial port for the Knomi_Serial device.

heater_hotend: extruder  # Name of the hotend heater.
heater_bed: heater_bed   # Name of the bed heater.

# Only one of heater_chamber and sensor_chamber should be configured.
heater_chamber:  # Name of the chamber heater.
sensor_chamber:  # Name of the chamber thermistor.

move_x: 10  # Number of millimeters to move the toolhead in the positive X direction.
move_y: 10  # Number of millimeters to move the toolhead in the positive Y direction.
move_z: 10  # Number of millimeters to move the toolhead in the positive Z direction.

speed_x: 100  # Speed to move the toolhead in the X direction.
speed_y: 100  # Speed to move the toolhead in the Y direction.
speed_z: 100  # Speed to move the toolhead in the Z direction.

gcodes:  # Comma separated G-Codes to display on the Knomi_Serial device.
```
