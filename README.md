# X4 Print Inbox

[![Tests](https://github.com/shelbeeely/x4-printer/actions/workflows/tests.yml/badge.svg)](https://github.com/shelbeeely/x4-printer/actions/workflows/tests.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Turns an [Xteink X4](https://www.xteink.com/) e-paper device into a
wireless print inbox and offline approval terminal. A Raspberry Pi Zero W
acts as a normal network printer that any OS's built-in print dialog can
find (no drivers), retains the original document, and converts it to the
X4's native page format. The X4 downloads new documents on wake, lets you
read and approve/reject them **fully offline**, and syncs the approval back
— directly on the home LAN, or through a lightweight cloud relay when
you're away from home.

```
Normal Print Dialog
        v
   Raspberry Pi
        v
retain original + generate XTC
        v
      X4 wakes
        v
downloads pending documents to SD
        v
     X4 goes offline
        v
user reads and approves a document
        v
approval is saved locally
        v
X4 later gets internet access
        v
approval syncs to home Pi directly or through relay
        v
Pi sends the original document to a physical network printer
```

See `docs/architecture.md` for the full design and `docs/protocol.md` for
the exact wire contract between the three components.

## Components

| Path | What it is |
|---|---|
| `firmware/` | X4 firmware: FreeInk-SDK-based sync client, offline reader, offline approval capture, deep-sleep scheduler |
| `pi-server/` | Raspberry Pi print server: IPP/mDNS printer, PDF→XTC conversion, durable job queue, device sync API, CUPS forwarding, relay client |
| `relay/` | Optional lightweight cloud relay for approving prints away from home (metadata only, never document bytes) |
| `tools/simulate_x4.py` | A fake X4 that speaks the real sync protocol, for exercising the whole pipeline without hardware |
| `tests/integration/` | End-to-end tests running real server instances against the fake X4 client |
| `docker/`, `docker-compose.test.yml`, `tests/docker/` | Local dev/test-only containers — real CUPS integration testing, see `docs/testing.md` |
| `firmware/test/wokwi_sync/` | Wokwi ESP32-C3 simulation harness for the on-device sync stack, see `docs/testing.md` |
| `docs/` | Architecture, protocol, format, setup, and testing documentation |

## Docs & flashing

**Project site (with browser-based firmware flashing):**
https://shelbeeely.github.io/x4-printer/ — this only goes live once the
repo's GitHub Pages source is set to "GitHub Actions" in Settings (a
one-time manual step).

## Quick start

1. **Pi**: `cd pi-server && sudo ./install/install.sh` — see
   `docs/setup-pi.md` for the full walkthrough (configuring your physical
   printer in CUPS, pairing a device).
2. **X4**: `git submodule update --init firmware/freeink-sdk && cd firmware
   && pio run -e xteink_x4 -t upload` — see `docs/setup-x4.md` for SD card
   provisioning and first boot.
3. **(Optional) Relay**, for approving prints away from home: `cd relay &&
   sudo ./install/install.sh` — see `docs/relay.md`.

Print from any computer on the network by selecting **"Xteink X4"** in the
normal print dialog — nothing else to configure on the client side.

## Running the tests

```sh
# Pi server unit tests
cd pi-server && python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt && pip install pytest
python -m pytest -q

# Relay unit tests
cd relay && python3 -m venv .venv && . .venv/bin/activate
pip install pytest && python -m pytest -q

# End-to-end integration tests (real IPP + sync API + relay servers,
# a fake CUPS `lp`, and the same client library tools/simulate_x4.py uses)
cd /path/to/repo && . pi-server/.venv/bin/activate
python -m pytest tests/integration -q

# Firmware host-side unit tests (pure-logic modules — no board needed)
cd firmware/test && cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The four suites above are green in this repository as committed and are
what "the tests" means day to day. Two more, slower layers exist for
deeper coverage and are documented separately in **`docs/testing.md`**:
a Docker Compose stack that runs `printer_forward.py`'s `lp` call against
a real CUPS daemon instead of a fake one, and a best-effort Wokwi
ESP32-C3 simulation that runs the real on-device Wi-Fi/sync firmware
against a real `pi-server` instance.

## Design highlights

- **Looks like a normal printer.** `pi-server` speaks IPP/1.1 well enough
  for Windows, macOS, Linux, Android, and iOS's built-in driverless print
  paths, advertised over mDNS — no app, no driver install.
- **Never loses a job or an approval.** Every durable write (Pi's SQLite
  job/approval tables, the X4's SD-backed job index and approval outbox) is
  atomic, and approval application is idempotent end-to-end via a
  client-generated `approval_id` — see `docs/protocol.md` §3. Retries,
  reboots, and dual delivery (direct + relay) can never cause a duplicate
  physical print.
- **Respects the ESP32-C3's memory budget.** Downloads stream straight to
  SD, pages stream straight from SD into the display framebuffer, and
  every on-device index is fixed-capacity — no full-document buffers
  anywhere in firmware. See `docs/architecture.md` "Memory budget".
- **Deep sleep by default.** The X4 is never remotely wakeable; it wakes
  from a button press or its own RTC timer, syncs, and goes back to sleep
  — a timer wake doesn't even light the screen if there's nothing to show
  a user who isn't there. See `docs/architecture.md`'s wake sequence.
- **Reads sharper, sideways.** Every job also gets a landscape-strip
  rendering split into panel-sized, pre-rotated pages — turn the device
  90° for text that uses the panel's full 800px dimension as reading
  length instead of the shorter 480px one. Toggle per-document from the
  reader's action menu; no format or firmware-rendering change needed to
  support it. See `docs/architecture.md` "Landscape-strip reading mode".
- **The relay never needs your documents.** It carries device/job IDs,
  actions, and timestamps — never the original file or the XTC preview
  (opt-in exception documented in `docs/relay.md`), and neither the Pi nor
  the X4 accepts an inbound connection for any of this: both only ever
  dial out.

## What's reused from the reference projects, and what isn't

Built on the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) as an
actual library dependency (not forked), informed by
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)'s
patterns for structuring a FreeInk application,
[paperlesspaper/paperlessprinter](https://github.com/paperlesspaper/paperlessprinter)'s
hand-rolled IPP wire protocol, and
[phrozen/xtx](https://github.com/phrozen/xtx)'s XTC/XTG format
specification. See `docs/architecture.md` for exactly what's reused versus
independently reimplemented from scratch, and `NOTICE` for attribution.

## License

MIT — see `LICENSE`.
