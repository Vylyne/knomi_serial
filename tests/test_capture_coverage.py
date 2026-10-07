#!/usr/bin/env python3
"""A screenshot needs every pixel, not a sum of overlapping flush areas."""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as temp:
        executable = os.path.join(temp, "capture_coverage.exe")
        built = subprocess.run(
            ["g++", "-std=c++11", "-Isrc", "tests/capture_coverage_cases.cpp",
             "-o", executable],
            cwd=ROOT, capture_output=True, text=True,
        )
        if built.returncode:
            print(built.stderr)
            return built.returncode
        result = subprocess.run(
            [executable], cwd=ROOT, capture_output=True, text=True,
        )
        if result.returncode:
            print(result.stderr)
        else:
            print("  1/1 passed")
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
