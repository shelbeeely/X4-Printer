#!/usr/bin/env bash
# Driver for firmware/. Two genuinely different things live here:
#
#   1. Host-side pure-logic unit tests (firmware/test/) -- real, C++17 +
#      CMake only, no ESP32 toolchain needed. This is the part that
#      actually builds and runs in a plain Linux sandbox.
#   2. A real ESP32-C3 device build/flash (`pio run`) -- documented here
#      honestly as usually blocked in a sandboxed agent environment; see
#      `try-device-build` below, which attempts it live and reports
#      whatever the real current failure is instead of asserting a
#      possibly-stale one.
#
# Usage:
#   .claude/skills/run-firmware/driver.sh test               # build + run host tests
#   .claude/skills/run-firmware/driver.sh try-device-build    # attempt a real pio build, report what happens
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "$HERE/../../.." && pwd)"

do_test() {
  cd "$FIRMWARE_DIR/test"
  cmake -B build || exit 1
  cmake --build build || exit 1
  ctest --test-dir build --output-on-failure
}

do_try_device_build() {
  cd "$FIRMWARE_DIR"
  if [ ! -f "freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h" ]; then
    echo "freeink-sdk submodule not populated -- run: git submodule update --init firmware/freeink-sdk" >&2
    exit 1
  fi
  if ! command -v pio >/dev/null 2>&1; then
    echo "PlatformIO not installed -- run: pip install platformio" >&2
    exit 1
  fi
  echo "Attempting a real ESP32-C3 build (env: xteink_x4)..."
  echo "If this fails during PlatformIO's own toolchain bootstrap (not this"
  echo "repo's code), see SKILL.md's Gotchas section for two known sandbox"
  echo "environment failure modes and their fixes: a GitHub archive-download"
  echo "scope issue fetching pioarduino/platformio-core, and a CA-bundle"
  echo "override bug in penv_setup.py that breaks later downloads' TLS trust."
  echo
  pio run -e xteink_x4
}

case "${1:-}" in
  test) do_test ;;
  try-device-build) do_try_device_build ;;
  *) echo "usage: $0 {test|try-device-build}" >&2; exit 1 ;;
esac
