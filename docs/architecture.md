# Architecture

## Goal

Turn an Xteink X4 e-paper device into a wireless print inbox and offline
approval terminal, backed by a Raspberry Pi Zero W acting as a normal
network printer (IPP/driverless, mDNS-discoverable) for every desktop and
mobile OS, with a lightweight cloud relay for remote approval when the X4 is
away from home.

```
                    ┌─────────────────────────────────────────────┐
                    │              Any OS print dialog             │
                    │   Windows / macOS / Linux / Android / iOS    │
                    └───────────────────────┬───────────────────────┘
                                             │ IPP / AirPrint / driverless
                                             │ (mDNS: "Xteink X4._ipp._tcp")
                                             ▼
┌───────────────────────────── Raspberry Pi Zero W ─────────────────────────────┐
│  ipp_server.py  ──▶  convert.py (PyMuPDF+Pillow)  ──▶  xtc_writer.py          │
│       │                     │                              │                  │
│       ▼                     ▼                              ▼                  │
│  originals/<job>.pdf   jobs.db (SQLite)              xtc/<job>.xtc            │
│                              │                                                │
│  sync_api.py  ◀── HTTPS :8443, device-token auth ────────────────────────────▶│
│  relay_client.py  ── outbound HTTPS poll ─────┐                               │
│  printer_forward.py  ── `lp -d <queue>` ──▶ CUPS ──▶ physical network printer │
└─────────────────────────────────────────────────┼─────────────────────────────┘
                                                    │ outbound HTTPS only
                                                    ▼
                                     ┌───────────────────────────┐
                                     │   Cloud relay (relay/)    │
                                     │  approval envelopes only  │
                                     │  (device/job IDs, no      │
                                     │   document bytes)         │
                                     └─────────────┬─────────────┘
                                                    │ outbound HTTPS (away from home)
                                                    ▼
                                     ┌───────────────────────────┐
                                     │        Xteink X4          │
                                     │  FreeInk SDK + FreeInkUI  │
                                     │  wakes → syncs → sleeps   │
                                     └───────────────────────────┘
```

## Component responsibilities

| Component | Role | Built on |
|---|---|---|
| `pi-server/` | Print server: IPP receiver, PDF→XTC conversion, durable job queue, device sync API, CUPS forwarding, relay polling | Pure-stdlib Python (`http.server`, `sqlite3`, `ssl`) + PyMuPDF + Pillow, IPP wire format modeled on `paperlesspaper/paperlessprinter`'s hand-rolled `BaseHTTPRequestHandler` IPP server |
| `firmware/` | On-device sync client, offline reader, offline approval capture, deep-sleep scheduler | FreeInk SDK (`EInkDisplay`, `SDCardManager`, `PowerManager`, `InputManager`, `FreeInkUI`/`FreeInkApp`), architecture patterned on `crosspoint-reader` (HAL usage, `PersistableStore`-style atomic JSON persistence, streaming HTTP downloader) |
| `relay/` | Store-and-forward for approval envelopes only, so the X4 can approve prints away from home without exposing the home network | Pure-stdlib Python, same style as `pi-server` |

## Why these reference projects, and where this project departs from them

- **FreeInk SDK** supplies every piece of X4 hardware abstraction this
  project needs — display facade, SD card, power/deep-sleep, buttons/touch,
  and the `FreeInkUI`/`FreeInkApp` screen-builder UI layer — so
  `firmware/` contains **zero** raw display-controller or GPIO code. See
  `firmware/platformio.ini`, modeled directly on FreeInk's
  `platformio.sample.ini` `[env:xteink_x4]`.
- **CrossPoint Reader** is the reference for *how* to use FreeInk in a real
  product: its `WifiCredentialStore`/`PersistableStore` pattern (JSON on SD,
  atomic write-then-rename, obfuscated-not-encrypted secrets, explicit
  `getFilePath()`/`toJson()`/`fromJson()` contract) is mirrored by this
  project's `DeviceConfig`, `WifiStore`, `JobStore`, and `ApprovalOutbox`
  (`firmware/src/store/`, `firmware/src/config/`). Its `HttpDownloader`
  (`downloadToFile` streaming to `HalFile`) is the model for
  `firmware/src/net/SyncClient.*`'s streamed, SHA-256-verified download.
  This project does **not** depend on CrossPoint's code or its `lib/`
  tree — it is a separate firmware application built directly on the SDK,
  because the print-inbox use case (small JSON-driven job list + approval
  actions) doesn't need CrossPoint's EPUB/dictionary/OPDS machinery, and
  pulling in CrossPoint's `src/` would bind this project to CrossPoint's own
  app structure instead of FreeInk's stable public API.
- **PaperlessPrinter** is the reference for making an IPP endpoint that
  every OS's built-in driverless print path accepts without a driver
  install: its byte-level IPP attribute encoding (`_ipp_attr*` helpers,
  `Get-Printer-Attributes` response shape, chunked-body handling,
  `document-format` sniffing) is reused almost verbatim in
  `pi-server/xteink_print_server/ipp_server.py`. This project's IPP server
  is **narrower** than PaperlessPrinter's: PaperlessPrinter renders the
  incoming document straight to display-native PNGs for an e-paper *client
  device to poll over HTTP*; this project instead (a) retains the original
  document bytes untouched (needed later to send the *original*, not a
  preview, to the physical printer) and (b) converts to XTC instead of PNG
  (needed for the X4's own local paging UI, not a browser-side viewer).
- **`phrozen/xtx`** is the authoritative XTC/XTG byte-layout reference. Its
  Go source is not linked into either the Pi server or the firmware —
  `xtc_writer.py` is a from-scratch pure-Python encoder against the same
  spec (so the Pi doesn't need a Go toolchain), and
  `firmware/src/xtc/XtcReader.*` is a from-scratch, RAM-bounded C++ reader
  (so the firmware doesn't need a general-purpose image decoder). See
  `docs/xtc-format.md` for the exact subset produced/consumed.

## Data model (Pi, SQLite `jobs.db`)

```
jobs(
  job_id TEXT PRIMARY KEY,       -- uuid4 hex
  title TEXT,
  created_at INTEGER,
  source TEXT,                   -- 'ipp'
  original_path TEXT,            -- originals/<job_id>.<ext>, untouched bytes
  original_mime TEXT,
  original_bytes INTEGER,
  xtc_path TEXT,                 -- xtc/<job_id>.xtc
  xtc_bytes INTEGER,
  xtc_sha256 TEXT,
  page_count INTEGER,
  status TEXT                    -- pending | archived | deleted
)

job_deliveries(                  -- per-device ack state (§1.3 of protocol.md)
  job_id TEXT, device_id TEXT, delivered_at INTEGER,
  PRIMARY KEY (job_id, device_id)
)

approvals(
  approval_id TEXT PRIMARY KEY,  -- device-generated uuid4 hex, the idempotency key
  device_id TEXT,
  job_id TEXT,
  action TEXT,                   -- print | keep | delete
  created_at INTEGER,            -- device-side timestamp
  received_at INTEGER,
  received_via TEXT,             -- 'direct' | 'relay'
  applied INTEGER,                -- 0/1
  applied_at INTEGER,
  detail TEXT,                   -- printed | print_failed | kept | archived
  cups_job_id INTEGER,
  error TEXT
)

devices(
  device_id TEXT PRIMARY KEY,
  token_hash TEXT,                -- sha256 of the bearer token, never store plaintext
  name TEXT,
  account_id TEXT,                -- relay account, nullable
  paired_at INTEGER,
  last_seen_at INTEGER
)
```

## Idempotent approval application

`approvals.approval_id` is the single idempotency key used everywhere (see
`docs/protocol.md` §3). The critical invariant: **applying an approval and
recording it as applied happen in one SQLite transaction.** Concretely,
`printer_forward.apply_approval()`:

1. `BEGIN IMMEDIATE`
2. `INSERT OR IGNORE INTO approvals (...)` — if a row already existed
   (duplicate), read it back, `COMMIT`, and return its stored result without
   touching CUPS.
3. Otherwise perform the side effect (`lp -d <queue> <original_path>` for
   `print`, or a status update for `keep`/`delete`).
4. `UPDATE approvals SET applied=1, detail=..., cups_job_id=... WHERE approval_id=...`
5. `COMMIT`

A crash between steps 3 and 4 is the only gap where a duplicate physical
print could theoretically occur (the `lp` invocation both submitted the job
*and* the transaction that would have recorded it never committed, so a
retry after restart resubmits). This is bounded and documented as the one
place true exactly-once cannot be guaranteed without a two-phase commit with
CUPS itself (which does not support that) — everywhere else, retries,
relay-vs-direct double delivery, and device reboots are fully idempotent.

## Deep sleep / wake sequence (firmware)

Implements the task exactly as specified:

1. Wake (button press or RTC timer, via `freeink::PowerManager` /
   `InputManager::getWakeupReason()`).
2. `WifiManager::connect()` — join the strongest known saved network
   (`WifiStore`), bounded timeout (`WIFI_CONNECT_TIMEOUT_MS`, default 15s);
   on failure, skip straight to step 8 (offline is a normal state, not an
   error).
3. `SyncManager::fetchPendingJobs()` — `GET /jobs?status=pending`.
4. `SyncManager::downloadPendingJobs()` — for each job,
   `SyncClient::downloadJobToSd()` streams the XTC body directly into an
   open `HalFile`-equivalent SD file handle in 2KB chunks (never buffering
   the whole file), computing SHA-256 incrementally with `mbedtls`'s
   streaming API.
5. `SyncClient::verifyAndAck()` — compare computed hash to
   `X-Content-SHA256`; on match, `POST /jobs/{id}/ack` and record the job in
   `JobStore` as `downloaded`; on mismatch, delete the partial file and
   leave the job `pending` for the next wake.
6. `SyncManager::drainApprovalOutbox()` — POST every unsynced
   `ApprovalOutbox` entry to the Pi (direct) or relay (fallback, if the Pi
   sync endpoint isn't reachable on the current network); mark `synced` on
   `applied`/`already_applied`, leave for retry on network failure.
7. (steps 3-6 repeat once more if new approvals were drained and the Pi
   might have new jobs as a result — bounded to 2 passes total, not
   unbounded, so a misbehaving server can't keep the radio on forever)
8. `WiFi.disconnect(true, true)` — radio off.
9. `freeink::PowerManager::powerDownRailsForSleep()` +
   `armPowerButtonWakeup()` + `armWakeOnPins()` (RTC alarm pin, if a timed
   wake is configured) → `deepSleep()`.

Between wake and sleep, if the user is actively interacting with the Print
Inbox UI (paging, approving), the sync sequence above only runs once at
boot; further approvals during the session are appended to the
`ApprovalOutbox` (durable, but not re-synced mid-session) and picked up on
the *next* wake, keeping Wi-Fi off for the rest of the interactive session —
"use deep sleep aggressively" and "the X4 does not need to be remotely
woken" both hold: nothing above requires an inbound connection to the X4 at
any point.

## On-device Web UI (opt-in)

Everything above holds for normal operation. `ui/WebUiServer.h`/`.cpp` adds
one deliberate, scoped exception: a manual "Web UI" button on the Inbox
screen's footer that lets you check/manage the queue from a phone browser
without touching the physical buttons — useful when the device is out of
easy reach, or (hotspot mode) away from any known network entirely.

- **Explicit, not automatic.** Pressing the button shows a choice — "Use
  Wi-Fi" (joins a saved network via the same `WifiManager::connect()` the
  normal sync pass uses) or "Use Hotspot" (`WifiManager::startAccessPoint()`
  broadcasts the device's own SoftAP) — never a silent fallback between
  them, so a brief home-Wi-Fi drop can't unexpectedly turn the device into
  a public hotspot.
- **Gated by a fresh PIN.** Every time it's turned on, a new 6-digit PIN is
  generated and shown on the e-ink screen; the phone enters it once
  (`POST /login`) and gets a random session cookie for the rest of that
  toggle-on session. Plain HTTP, no TLS — see `docs/security.md` "On-device
  Web UI" for why that's an accepted tradeoff here.
- **Reuses the real approval path.** `POST /api/jobs` on the phone goes
  through the exact same `store::enqueueApproval()` (`ApprovalOutbox.h`)
  the physical action menu uses — durable-outbox-entry-before-any-network-
  attempt, the same idempotent guarantees, not a parallel implementation.
- **Bounded exposure window.** The server and whichever radio mode is
  active are torn down by the same idle timer that governs the rest of the
  UI (`main.cpp`'s `kIdleSleepTimeoutMs`) — an actively-browsing phone's
  periodic status poll counts as activity, an idle one doesn't, and
  `goToSleep()` defensively stops the web UI before every deep sleep
  regardless of how it was left running.

## Memory budget (ESP32-C3, firmware)

The C3 has 400KB SRAM total, shared between the Wi-Fi/TLS stack, FreeRTOS,
the FreeInk display framebuffer, and application code. Concretely:

| Consumer | Bound | Why |
|---|---|---|
| Display framebuffer | 48,000 B (800x480 / 8, single-buffer mode, `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1`) | Fixed, owned by FreeInk |
| XTC page render | 2,048 B chunk buffer | `XtcReader` streams file→framebuffer in fixed chunks, §`docs/xtc-format.md` |
| SD download | 2,048 B chunk buffer | `SyncClient::downloadJobToSd()` streams HTTP→SD in fixed chunks; SHA-256 state is ~200 B, not proportional to file size |
| Job/outbox index | Bounded by `MAX_INBOX_JOBS` (64) and `MAX_OUTBOX_ENTRIES` (32) fixed-capacity JSON arrays, loaded once at boot (~4KB typical) | `JobStore`/`ApprovalOutbox` refuse to grow past these caps; the UI surfaces "inbox full, archive or delete something" rather than allocating unbounded state |
| Wi-Fi + TLS (esp_http_client/mbedTLS) | ~40-60KB while connected | Only resident during the sync window (steps 2-8 above); torn down before deep sleep |
| Web UI (Wi-Fi/SoftAP + `WebServer`, no TLS) | Similar order of magnitude to the sync-window Wi-Fi row above, minus the TLS overhead | Optional — only resident while the "On-device Web UI" feature is manually toggled on; torn down by the same idle timer as the rest of the UI, never during normal (button/timer-wake) operation |

No component ever holds a full downloaded document, a full XTC file, or more
than one rendered page in RAM at once — the one and only large fixed buffer
is the framebuffer itself, which FreeInk already owns regardless of this
project.

## Security model (summary — full detail in `docs/security.md`)

- **Pi sync API**: TLS (self-signed, CA pinned on the SD card at pairing
  time) + per-device bearer token. Not exposed beyond the LAN; no port
  forwarding, no inbound rule needed on the home router.
- **Relay**: TLS (public CA, e.g. Let's Encrypt on whatever host the user
  deploys it to) + per-account bearer token. Carries only IDs/actions/
  timestamps by default (§2.5 of `docs/protocol.md` documents the opt-in
  exception). The relay cannot cause a print on its own — it can only
  relay an approval envelope that the Pi independently re-validates
  (device token check happens again at the Pi when it applies the
  approval) before ever touching CUPS.
- **CUPS**: the physical printer queue is configured once by the installer
  (`pi-server/install/install.sh`); `printer_forward.py` only ever shells
  out to `lp -d <configured-queue>`, never to an arbitrary queue name
  supplied by a request.
- **On-device Web UI**: off by default, manually toggled, PIN-gated, plain
  HTTP (no TLS) — see "On-device Web UI (opt-in)" above and
  `docs/security.md` for the full tradeoff writeup. Unlike everything else
  in this list, this one *does* accept an inbound connection — deliberately
  and only while a person has just turned it on.
