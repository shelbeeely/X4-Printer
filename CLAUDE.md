# CLAUDE.md

Guidance for Claude Code sessions working in this repository.

## Project summary

Focusink lets an Xteink X4 e-paper device act as a wireless, offline
print-approval terminal. A Raspberry Pi Zero W poses as a normal
IPP/mDNS-discoverable network printer, keeps the original document bytes,
and converts each job to the X4's native XTC page format. The X4 pulls
pending jobs to its SD card on wake, lets the user page through and
approve/reject them with the radio off, then pushes the approval back to
the Pi — either directly on the LAN or via an optional store-and-forward
cloud relay — which is the only thing that triggers the actual `lp` print
to a physical printer via CUPS. Three independent components (firmware,
pi-server, relay) talk over a small HTTP/JSON wire protocol; no component
trusts another's retries not to duplicate.

## Components

| Path | What it is | Key entry points |
|---|---|---|
| `firmware/` | ESP32-C3 firmware for the X4: sync client, offline reader/approval UI, deep-sleep scheduler. Built on the FreeInk SDK (external library dependency, not vendored). | `firmware/src/main.cpp`, `firmware/src/sync/SyncManager.*`, `firmware/src/net/SyncClient.*`, `firmware/src/store/{JobStore,ApprovalOutbox}.*`, `firmware/platformio.ini` |
| `pi-server/` | Raspberry Pi print server: IPP/mDNS printer endpoint, PDF→XTC conversion, SQLite job queue, device sync API, CUPS forwarding, relay client. | `pi-server/focusink_server/{server,ipp_server,convert,xtc_writer,db,sync_api,printer_forward,relay_client}.py` |
| `relay/` | Optional cloud relay for approving prints away from home; carries approval envelopes (device/job IDs, actions, timestamps) only, never document bytes. | `relay/relay_server/{app,server,db}.py` |
| `tools/simulate_x4.py` | Fake X4 client speaking the real sync protocol, used by integration tests (and available standalone) to exercise the Pi/relay without hardware. | `tools/simulate_x4.py` |
| `tests/integration/` | End-to-end tests: real IPP + sync API + relay server instances, a fake CUPS `lp`, and `tools/simulate_x4.py` driving the full pipeline. | `tests/integration/test_end_to_end.py`, `tests/integration/conftest.py` |
| `docs/` | Design, protocol, format, and setup documentation — see "Docs map" below. | `docs/architecture.md`, `docs/protocol.md` |

## Build / test commands

Verified against this repository as checked out; run each from the repo root unless noted.

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

Firmware build/flash (requires the `firmware/freeink-sdk` git submodule —
`firmware/platformio.ini`'s `lib_deps` resolve via `symlink://freeink-sdk/...`
relative to `firmware/`, matching the submodule's path. A plain `git clone`
of this repo does not populate submodule contents; run `git submodule
update --init firmware/freeink-sdk` first, or clone with
`--recurse-submodules`):

```sh
cd firmware && pio run -e xteink_x4            # build
cd firmware && pio run -e xteink_x4_release     # release build
cd firmware && pio run -e xteink_x4 -t upload   # flash
```

No lint or CI commands exist in this repository as of this writing — don't
invent or assume them.

Two additional, slower test layers exist beyond the four commands above —
see `docs/testing.md` for what each does and how to run it:

```sh
# Real-CUPS integration test (Docker Compose: pi-server + relay + a real
# CUPS daemon with a virtual PDF-backed printer)
docker compose -f docker-compose.test.yml build cups pi-server
docker compose -f docker-compose.test.yml up -d cups   # wait for it to report healthy
docker compose -f docker-compose.test.yml run --rm pi-server python -m pytest tests_docker -q
docker compose -f docker-compose.test.yml down -v

# Wokwi ESP32-C3 simulation of the real on-device sync stack (best-effort,
# needs a WOKWI_CLI_TOKEN — see docs/testing.md before relying on this)
cd firmware && pio run -e wokwi_sync_test
```

## Invariants

- **Atomic durable writes.** Every durable write — Pi-side SQLite
  (`jobs.db`) and X4-side SD-backed JSON stores (`JobStore`,
  `ApprovalOutbox`, `WifiStore`, `DeviceConfig`) — uses a write-then-rename
  pattern. Why: a crash or power loss mid-write must never leave a
  corrupt/partial job queue or approval outbox on either side. See
  `docs/architecture.md` "Data model" and `firmware/src/store/AtomicJsonFile.*`.

- **Idempotent approval application.** `approvals.approval_id` is a
  client-generated (X4-side) idempotency key used end-to-end
  (`docs/protocol.md` §3). Applying an approval and recording it as applied
  happen in one SQLite transaction, in `printer_forward.apply_approval()`.
  Why: approvals can arrive twice (direct + relay dual delivery, network
  retries, device reboot mid-sync) and must never cause a duplicate
  physical print. See `docs/architecture.md` "Idempotent approval
  application" for the exact transaction steps and the one documented gap
  (a crash between the `lp` call and the commit).

- **Firmware memory budget.** The ESP32-C3 has 400KB SRAM total; no
  component holds a full document, a full XTC file, or more than one
  rendered page in RAM. All buffers are fixed-capacity: `MAX_INBOX_JOBS`
  (64) and `MAX_OUTBOX_ENTRIES` (32) bound the on-device job/outbox
  indexes, and downloads/page renders stream through 2KB chunk buffers.
  Why: there is no headroom for unbounded or document-sized allocations
  alongside the Wi-Fi/TLS stack and display framebuffer. See
  `docs/architecture.md` "Memory budget" table.

- **X4 never accepts inbound connections.** The device only ever dials out
  (sync fetch, downloads, approval POSTs); it wakes solely from a button
  press or its own RTC timer, never remotely. Why: this is a deliberate
  security property — no listening socket on the device means nothing on
  the network can reach it, wake it, or push data to it. Don't design
  features that need inbound connectivity to the X4. See
  `docs/architecture.md` "Deep sleep / wake sequence" and
  `docs/security.md`.

- **CUPS queue is fixed at install time.** `printer_forward.py` only ever
  shells out to `lp -d <configured-queue>`, where the queue name comes from
  install-time configuration, never from a request. Why: prevents command
  injection or queue-redirection via a crafted approval/job. See
  `pi-server/focusink_server/printer_forward.py` and
  `docs/architecture.md` "Security model".

## Docs map

- `docs/testing.md` — every test layer in this repo (unit, integration,
  firmware host tests, the Docker real-CUPS integration test, the
  best-effort Wokwi on-device sync simulation), how to run each, and
  what's still a known gap.
- `docs/architecture.md` — full system design: component responsibilities,
  reference-project attribution and departures, SQLite data model,
  idempotent-approval transaction, wake/sleep sequence, memory budget,
  security model summary.
- `docs/protocol.md` — wire-level contract between firmware, pi-server, and
  relay: every HTTP endpoint, request/response shape, and the idempotency
  summary (§3).
- `docs/relay.md` — what the optional cloud relay is for, what it does and
  doesn't carry, and how to deploy it.
- `docs/security.md` — threat model and what is/isn't covered (this is a
  prototype for home/personal-scale deployment, not a hardened multi-tenant
  product).
- `docs/setup-pi.md` — walkthrough for provisioning the Raspberry Pi print
  server (Docker Compose, the recommended path, or a manual systemd
  install), configuring the physical printer in CUPS, and pairing a
  device.
- `docs/setup-x4.md` — building/flashing the firmware and provisioning the
  X4's SD card for first boot.
- `docs/xtc-format.md` — the XTC/XTG byte-layout subset this project
  produces (Pi) and consumes (firmware).
