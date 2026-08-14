"""PlatformIO target: write one flashable image.

    pio run -e knomi -t mergebin

`pio run -t upload` writes four files at four offsets — bootloader, partition
table, `boot_app0`, and the app. Anything flashing a *published* image has to
reproduce all four at those same offsets, and getting one wrong does not fail
loudly: a board with no bootloader comes up dark, which reads as dead hardware
rather than as a bad flash.

So this merges the four into a single image that flashes at one offset, and
there is nothing left to get wrong:

    esptool --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 knomi-<version>.bin

The offsets are taken from the build environment rather than restated here.
They are a property of the chip and of `knomi_partitions.csv`, both of which
can move under a platform bump, and a hardcoded copy would go on producing
images that flash cleanly and boot into nothing.

Also writes `manifest.json` beside the image, which is what ESP Web Tools reads
to flash the same file from a browser — the path onto new hardware that has
never had a toolchain pointed at it.
"""

import json
import os
import subprocess

Import("env")

# ESP Web Tools names families this way; PlatformIO names the MCU the other.
_CHIP_FAMILY = {
    "esp32": "ESP32",
    "esp32c3": "ESP32-C3",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
}


def _flash_freq(board):
    """`80000000L` as the board file spells it, `80m` as esptool wants it."""
    raw = str(board.get("build.f_flash", "80000000L")).rstrip("Ll")
    return f"{int(raw) // 1000000}m"


def _parts(env):
    """Every image the upload writes, as (offset, path), lowest first."""
    parts = [
        (int(str(offset), 0), str(path))
        for offset, path in env.get("FLASH_EXTRA_IMAGES", [])
    ]
    parts.append(
        (
            int(str(env.get("ESP32_APP_OFFSET", "0x10000")), 0),
            env.subst("$BUILD_DIR/${PROGNAME}.bin"),
        )
    )
    parts.sort()
    return parts


def _run_esptool(env, chip, out, flash_mode, flash_freq, flash_size, parts):
    """esptool 5 renamed the subcommand and the flags; 4 is what ships today.

    Try what is current, fall back to what is not, so a platform bump that
    carries a new esptool does not turn into a release that cannot be built.
    """
    esptool = env.subst("$OBJCOPY")
    python = env.subst("$PYTHONEXE")
    flat = [arg for offset, path in parts for arg in (hex(offset), path)]

    dialects = (
        ("merge_bin", "--flash_mode", "--flash_freq", "--flash_size"),
        ("merge-bin", "--flash-mode", "--flash-freq", "--flash-size"),
    )

    last = None
    for subcommand, mode_flag, freq_flag, size_flag in dialects:
        argv = [
            python, esptool,
            "--chip", chip,
            subcommand,
            "-o", out,
            mode_flag, flash_mode,
            freq_flag, flash_freq,
            size_flag, flash_size,
            *flat,
        ]
        result = subprocess.run(argv, capture_output=True, text=True)
        if result.returncode == 0:
            return
        last = result

    raise SystemExit(
        f"merge_bin: esptool failed in both dialects\n{last.stdout}\n{last.stderr}"
    )


def merge_bin(*_args, **_kwargs):
    board = env.BoardConfig()
    chip = board.get("build.mcu", "esp32s3")
    version = env.get("KNOMI_VERSION", "0.0.0")

    build_dir = env.subst("$BUILD_DIR")
    image = os.path.join(build_dir, f"knomi-{version}.bin")
    parts = _parts(env)

    for _offset, path in parts:
        if not os.path.isfile(path):
            raise SystemExit(f"merge_bin: nothing at {path}")

    print("merge_bin: writing one image from")
    for offset, path in parts:
        print(f"  {hex(offset):>10}  {os.path.basename(path)}")

    _run_esptool(
        env,
        chip=chip,
        out=image,
        flash_mode=board.get("build.flash_mode", "qio"),
        flash_freq=_flash_freq(board),
        flash_size=board.get("upload.flash_size", "16MB"),
        parts=parts,
    )

    # The offset is 0 because the merge already placed everything; a web
    # flasher writes this one file and nothing else.
    manifest = {
        "name": "Knomi_Serial",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": _CHIP_FAMILY.get(chip, chip.upper()),
                "parts": [{"path": os.path.basename(image), "offset": 0}],
            }
        ],
    }
    with open(os.path.join(build_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"merge_bin: {image}")


env.AddCustomTarget(
    name="mergebin",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=[merge_bin],
    title="Merged image",
    description="One flashable image, plus the manifest a web flasher reads",
)
