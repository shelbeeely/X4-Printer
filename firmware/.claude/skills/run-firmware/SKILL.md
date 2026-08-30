---
name: run-firmware
description: Build and test the X4 firmware. Use when asked to build firmware, run firmware tests, flash the X4, or check whether a real ESP32 device build is possible in this environment.
---

Firmware here splits into two genuinely different things: **host-side
pure-logic unit tests** (real, build and run in any plain Linux sandbox, no
special hardware/toolchain) and **a real ESP32-C3 device build/flash**
(needs PlatformIO's full toolchain bootstrap — see Gotchas for two
environment-specific failure modes this has hit before and how they were
fixed; use `try-device-build` to get the *current* real status rather than
trust this doc if your environment differs).

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
xteink_x4_release`) — see Gotchas for the two sandbox-specific failure
modes this can hit before it ever reaches this repo's own code, and what
fixed them last time.

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
happens. Don't trust a cached memory of "device builds don't work here"
over actually running this — environments differ. As of this writing, in
this project's own sandbox, a full real build (compile + link + flash
image, both `xteink_x4` and `xteink_x4_release`) succeeds end to end.

| command | what it does |
|---|---|
| `driver.sh test` | cmake configure + build + ctest for the 3 host-side test binaries |
| `driver.sh try-device-build` | Attempt a real ESP32 build, report the real outcome |

## Run (human path)

```bash
pio run -e xteink_x4            # build
pio run -e xteink_x4 -t upload  # flash over USB
```

## Test

Same as "Run (agent path)"'s `driver.sh test` — there is no separate
build vs. test step for the host-side suite.

## Gotchas

- **Two sandbox-specific bootstrap failures, both diagnosed and fixed in
  this environment — worth knowing if a *different* or *fresh* sandboxed
  session hits the same symptoms**, since both live in PlatformIO's own
  toolchain state (`~/.platformio/...`), not in this repo, so they don't
  travel with a fresh container or a different session's GitHub scope:
  1. **`Failed to install Python dependencies into penv`**: PlatformIO's
     own internal bootstrap (`~/.platformio/platforms/espressif32/builder/penv_setup.py`)
     needs to `uv pip install` a dependency from
     `https://github.com/pioarduino/platformio-core/archive/refs/tags/<version>.zip`
     — a **different** GitHub repo than this one, fetched via a GitHub
     *archive/codeload* endpoint. If the session's GitHub access doesn't
     cover that repo's archive downloads specifically (plain `git
     clone`/`git fetch` of the same repo can work fine even when this
     fails — it's the archive-zip endpoint that's the narrower one), that
     request 403s and the build cannot proceed. Confirmed independent of
     `pio` with a bare `curl` to that exact URL. Fix that worked here:
     clone `pioarduino/platformio-core` at the pinned tag via plain `git
     clone` (which worked), then edit `penv_setup.py`'s
     `python_deps["platformio"]` value to a `file://` path pointing at
     that local clone instead of the GitHub archive URL.
  2. **`SSL: CERTIFICATE_VERIFY_FAILED: self-signed certificate in
     certificate chain`** on a *later* download (e.g. the ESP32 core
     toolchain tarball from `espressif/arduino-esp32`'s releases), even
     though the exact same URL succeeds when fetched standalone outside
     `pio run`. Root cause: `penv_setup.py`'s `_setup_certifi_env()`
     overwrites `REQUESTS_CA_BUNDLE`/`SSL_CERT_FILE`/`CURL_CA_BUNDLE`/
     `GIT_SSL_CAINFO` in-process with the *bare* `certifi` bundle right
     after the penv bootstrap succeeds — which drops any proxy/custom CA
     the session's outbound network relies on (e.g. a TLS-terminating
     egress proxy) for every download for the rest of that `pio run`
     process. Fix that worked here: patch `_setup_certifi_env()` to merge
     the extra CA bundle (path from `/root/.ccr/ca-bundle.crt` in this
     environment) into a copy of the certifi bundle instead of replacing
     trust outright, then point the env vars at the merged file.
  - Both fixes are local PlatformIO installation state, not part of this
    repo, and won't survive a fresh container — if you hit either failure
    again, the fix is the same; this doc exists so you don't have to
    re-diagnose the SSL one from scratch (it's non-obvious: the exact same
    download code path succeeds outside `pio run` and fails inside it,
    because the CA env vars only get clobbered mid-process).
- **Real firmware bugs found and fixed once a real device build finally
  got this far** (these ARE committed to the repo, unlike the two
  environment fixes above):
  - `namespace sync { ... }` in `src/sync/SyncManager.{h,cpp}` collided
    with the global `::sync()` from ESP-IDF's newlib `unistd.h` (pulled in
    transitively via `Arduino.h`) — host-side tests never caught this
    because they don't include `Arduino.h` at all. Renamed to
    `namespace syncmgr`.
  - `src/net/SyncClient.cpp` used `WiFiClient` without including
    `<WiFiClient.h>` directly, relying on an implicit transitive include
    via `<WiFiClientSecure.h>` that doesn't hold in this platform version.
    Added the explicit include.
  - The default partition table (`default.csv`, ~1.25MB per OTA app slot)
    is sized for a 4MB flash chip; this board's actual chip is 16MB
    (`board_build.flash_size = 16MB` in `platformio.ini`) and the firmware
    image had grown past the small default slot. This project has no OTA
    update path (flashed over USB only, per `docs/architecture.md`), so
    `platformio.ini` now sets `board_build.partitions = max_app_4MB.csv`
    (single large no-OTA app partition) instead of relying on the
    dual-OTA-slot default.

## Troubleshooting

- **`Failed to install Python dependencies into penv`** /
  **`self-signed certificate in certificate chain`**: see Gotchas above.
- **`freeink-sdk submodule not populated`**: run `git submodule update
  --init firmware/freeink-sdk` from the repo root (or `--recurse-submodules`
  on the original clone).
- **`Flash: ... used ... from ... FAILED`** (image too big for its
  partition): check `board_build.partitions` in `platformio.ini` against
  the actual flash chip size and whether OTA is genuinely needed; see the
  partition-table Gotcha above for the reasoning already applied here.
