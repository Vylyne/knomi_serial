#!/usr/bin/env python3
"""Exercise the firmware's accent-ink calculation without display hardware."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as temp:
        executable = os.path.join(temp, "theme_contrast.exe")
        command = [
            "g++", "-std=c++11", "-Isrc", "tests/theme_contrast_cases.cpp",
            "-o", executable,
        ]
        built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if built.returncode:
            print(built.stderr)
            return built.returncode
        result = subprocess.run([executable], cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            print(result.stderr)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
