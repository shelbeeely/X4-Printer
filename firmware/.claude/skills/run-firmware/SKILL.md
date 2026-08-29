---
name: run-firmware
description: Build and test the X4 firmware. Use when asked to build firmware, run firmware tests, flash the X4, or check whether a real ESP32 device build is possible in this environment.
---

Firmware here splits into two genuinely different things, and this skill
is honest about which one actually works in a sandboxed/headless agent
environment: **host-side pure-logic unit tests** (real, build and run
today, no special hardware/toolchain) vs. **a real ESP32-C3 device
build/flash** (blocked in most sandboxed sessions — see Gotchas, and use
`try-device-build` to get the *current* real status rather than trust
this doc if your environment differs).

All paths below are relative to `firmware/`.

## Prerequisites

For host tests: a C++17 compiler + CMake — nothing else (deliberately, see
`test/CMakeLists.txt`'s own comment: "no GoogleTest fetch so it runs
offline").

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake
```

For attempting a device build: the `freeink-sdk` git submodule populated,
and PlatformIO:

```bash
git submodule update --init freeink-sdk   # from firmware/
pip install platformio
```

## Build

Host tests build via CMake (see Run below — build and run are one step
with `ctest`).

A device build is `pio run -e xteink_x4` (release: `-e
xteink_x4_release`) — see `try-device-build` below for why this usually
doesn't get past its own dependency bootstrap in a sandboxed session.

## Run (agent path)

```bash
.claude/skills/run-firmware/driver.sh test
```

Configures + builds + runs the three host-side test binaries
(`XtcFormatTest`, `JobStoreTest`, `ApprovalOutboxTest` — pure-logic modules
with no Arduino/FreeInk dependency, see each module's own header comment
for why it was split out this way). All 3 pass today.

```bash
.claude/skills/run-firmware/driver.sh try-device-build
```

Attempts a real `pio run -e xteink_x4` and prints whatever actually
happens — including a live-checked note about the specific failure mode
this hits in a GitHub-access-scoped agent session (see Gotchas). Don't
trust a cached memory of "device builds don't work here" over actually
running this — environments differ and this session's scope could change.

| command | what it does |
|---|---|
| `driver.sh test` | cmake configure + build + ctest for the 3 host-side test binaries |
| `driver.sh try-device-build` | Attempt a real ESP32 build, report the real outcome |

## Run (human path)

On a machine with the actual ESP32 toolchain reachable (i.e. not blocked
by the GitHub-scope issue below):

```bash
pio run -e xteink_x4            # build
pio run -e xteink_x4 -t upload  # flash over USB
```

## Test

Same as "Run (agent path)"'s `driver.sh test` — there is no separate
build vs. test step for the host-side suite.

## Gotchas

- **A real device build/flash is blocked in most sandboxed agent
  sessions, for a subtle reason that has nothing to do with this repo.**
  `freeink-sdk` (this project's own git submodule dependency) clones fine.
  `pip install platformio` works fine. `pio run` even gets far enough to
  download the ESP32 platform package. It fails at **PlatformIO's own
  internal bootstrap** (`penv_setup.py`), which needs to `uv pip install`
  a dependency from `https://github.com/pioarduino/platformio-core/...` —
  a **different** GitHub repo than this one. If the current session's
  GitHub access is scoped to only this repo (common for agent sessions
  with repo-scoped tokens), that specific request gets a 403 with body
  `{"message":"GitHub access to this repository is not enabled for this
  session..."}`, and the build cannot proceed. Confirmed by reproducing
  the identical 403 with a bare `curl` to that exact URL, independent of
  `pio` entirely. If you hit this, don't keep retrying — it won't
  resolve itself; either broaden this session's GitHub access to include
  `pioarduino/*`, or build on an unscoped machine.
- **This is a session-scope limitation, not a `freeink-sdk`/toolchain
  problem** — worth stating explicitly because the failure message
  (`Failed to install Python dependencies into penv`) gives no hint that
  the real cause is a GitHub permissions boundary two layers down.

## Troubleshooting

- **`Failed to install Python dependencies into penv`**: see the Gotcha
  above — check `curl -sS https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip`
  (adjust the version tag to whatever `platform-espressif32`'s
  `penv_setup.py` currently pins) for the exact 403 body if you want to
  confirm it's the same cause.
- **`freeink-sdk submodule not populated`**: run `git submodule update
  --init firmware/freeink-sdk` from the repo root (or `--recurse-submodules`
  on the original clone).
