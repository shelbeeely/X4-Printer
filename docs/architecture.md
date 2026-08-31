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

## Landscape-strip reading mode

The X4's panel is 800x480 (landscape), but most print jobs are portrait
documents — fitting a whole portrait page into that box
(`xtc_writer.prepare_page_image()`, the default `RenderMode.FIT_PAGE`) is
bound by the panel's *shorter* 480px dimension, wasting most of the 800px
width as unused margin. `xtc_writer.prepare_landscape_strip_images()`
(`RenderMode.LANDSCAPE_STRIPS`) offers an alternative: render each source
page at a scale where its width maps to the panel's 480px dimension, slice
the result into consecutive panel-width-tall chunks, and pre-rotate each
one 90 degrees — so reading it means physically turning the device
sideways, but every strip uses the panel's full 800px dimension as reading
length instead of being bound by the shorter one.

This needed **no XTC/XTG format change** (the container already supports
an arbitrary number of independently-sized pages, see
`docs/xtc-format.md`) and **no firmware rendering change** (each strip is
already exactly panel-sized and correctly oriented, so
`XtcReader::renderPageToFramebuffer()`'s existing raw-copy fast path
handles it — firmware never scales or rotates anything itself). The
rotation direction (`Image.ROTATE_270` in `xtc_writer.py`) is a best-guess
convention, not verified against real hardware — there is no IMU on the X4
(BoardConfig's IMU capability is X3/Sticky-only) and no way to confirm
which physical edge holds the buttons once the device is turned sideways
without an actual unit; it's isolated to one call site for a trivial fix
if wrong.

`ipp_server.py`'s `_ingest_document()` **always attempts both renderings**
for every job — the Pi generates a landscape-strip variant alongside the
normal one, not on request. A landscape-conversion failure (e.g. a page
shape needing more strips than `prepare_landscape_strip_images`'s
`max_strips` guard allows) is logged and degrades to "no landscape variant
for this job," same as a thumbnail-generation failure — it never blocks
ingestion, since the normal rendering already succeeded. The second XTC
file is tracked by four more `jobs` columns
(`xtc_landscape_path`/`_bytes`/`_sha256`/`_page_count`, added via the same
`_ensure_column()` migration pattern `thumbnail_path` established), empty
path meaning "none" — the X4-side `JobStore` mirrors the same four fields
on `JobEntry`.

This doubles per-job Pi conversion time and X4 SD storage — an explicit,
known tradeoff (the user's own choice over two other designs: a
per-device default, or a second IPP printer queue) rather than an
oversight. The wake sequence's steps 3-5 above extend accordingly: the
job-listing manifest (`docs/protocol.md` §1.1) includes the landscape
variant's hash/size/page-count only when one exists, `SyncManager`
downloads and verifies it as a second file (`/inbox/<job_id>_l.xtc`)
**all-or-nothing** with the normal one — a job only becomes visible
on-device once every variant the manifest advertised is fully verified on
SD — and a single `POST /jobs/{id}/ack` covers both hashes at once
(§1.3), never a separate per-variant delivery state.

On-device, the reader screen (`InboxUI.cpp`) defaults to the normal view
for every freshly opened document; the action menu (opened via the page
counter) gains a "View: Landscape"/"View: Portrait" toggle row, shown only
for jobs that have a landscape variant, which reopens the same document
from the other file and resets to its own page 1 — there's no attempt to
map "roughly the same spot" between the two renderings, since they don't
share a page correspondence.

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

### On-device Web UI full-document preview

The job-list page's "Preview" button (`ui/pages/joblist.html`, see
`tools/xtc-wasm/README.md`) decodes the X4's own converted XTC bitmap —
useful everywhere, including hotspot mode, but it's the e-ink rendition,
not the original document. In **station mode only** (the X4 joined to a
real network, not its own isolated hotspot), the job-list page also shows
a "View full document" link per job that opens the Pi's untouched
original (`pi-server`'s `originals_dir` — see `docs/architecture.md`
"Data model") directly in the phone's browser.

This is a deliberately different shape from the WASM preview, not an
extension of it:

- **The X4 never touches these bytes.** The link points the phone straight
  at the Pi's admin console (`GET /api/admin/v1/jobs/{id}/original`,
  `admin_api.py`) — a normal top-level navigation/new-tab open, not a
  fetch the X4 proxies. Nothing is downloaded to, cached on, or streamed
  through the device; the whole point is that the original never needs to
  fit in the X4's flash or RAM budget the way an XTC page does.
- **Auth is the existing admin console password, reused as-is** — no new
  token-issuance mechanism. The Pi's `GET /api/admin/v1/jobs/{id}/original`
  route (`admin_api.py`) is gated by the exact same shared HTTP Basic
  check every other admin-console route already uses; the browser's own
  native password prompt handles it, so neither the X4 nor this page needs
  any client-side auth code for it. This only works at all when the Pi
  owner has set an admin password (`XTEINK_ADMIN_PASSWORD`) — if not, the
  link is simply never shown (see below), the same "empty disables"
  pattern the admin console and relay already use.
- **Wired at pairing time, not discovered at runtime.** `pair_device.py`
  writes an optional `pi_admin_base_url` field into `/system/device.json`
  only when the admin console is enabled at pairing time (mirroring how
  the relay fields are only written when relay is configured) —
  `config::DeviceConfigData::hasAdminConsole`/`piAdminBaseUrl`. `/api/status`
  only ever includes `pi_admin_base_url` in its response when
  `hasAdminConsole` is set *and* the server is currently in station mode —
  hotspot mode's phone has no network path to the Pi at all, so the field
  (and the link) is omitted rather than shown-but-broken.

Two more station-mode-only extensions reuse this exact shape — same
password gate, same non-proxied direct link, same `piAdminBaseUrl`-presence
gating:

- **Job thumbnails.** `convert.py`'s `render_thumbnail_jpeg()` generates a
  small JPEG from the job's own already-rendered, already-dithered first
  XTC page (no second document render) at ingest time
  (`ipp_server.py`'s `_ingest_document()`); a thumbnail-generation failure
  is logged and simply leaves the job without one, never blocking
  ingestion. Stored at `pi-server`'s `thumbnails_dir` and tracked by
  `jobs.thumbnail_path` (`db.py`'s `SCHEMA`, added via an explicit
  `_ensure_column()` migration — this project's first schema change to an
  already-deployed table; see that function's docstring for what it does
  and doesn't cover). Served by `GET /api/admin/v1/jobs/{id}/thumbnail`
  and rendered as a plain `<img>` per job card in `joblist.html`, with
  `onerror` removing the element on any failure (unknown job, no
  thumbnail generated, network failure) — the same "omit rather than
  show broken" rule as everywhere else in this feature.

  One open question, not fully resolved: unlike the "View full document"
  `<a target="_blank">` link (a top-level navigation, which reliably
  triggers the browser's native HTTP Basic Auth prompt on first use), an
  `<img>` is a subresource load. Verified in this project's own testing
  (a real headless Chromium, not just reasoning): a subresource request to
  an unauthenticated Basic-Auth origin fails cleanly with no prompt and no
  hang — so there's no risk of a jarring, unexplained password dialog
  appearing just because a thumbnail tried to load. What's *not* verified
  on real mobile browsers is whether completing that native prompt once
  (by opening "View full document") leaves credentials cached broadly
  enough that a *subsequent* `<img>` load — possibly in a different tab —
  succeeds silently afterward, the way HTTP Basic Auth caching has
  classically worked. If it doesn't hold on a given browser, the practical
  effect is simply "no thumbnail appears" — never a broken image, an
  incorrect one, or an unexpected prompt.
- **Recent activity.** `db.py`'s `list_recent_approvals_for_device()`
  (using the existing `idx_approvals_device` index) backs
  `GET /api/admin/v1/devices/{id}/approvals` — scoped to one device's own
  approval history (not the whole household's), reusing `device_id` from
  `/api/status`. Unlike "View full document" and the thumbnails, this route
  is now CORS-enabled (`admin_api.py`'s `_send_cors_headers()` plus a
  `do_OPTIONS` preflight handler, both scoped to this one route only), so
  `joblist.html` renders the approval history inline via its own
  `fetch(url, { credentials: "include" })` instead of only linking out. The
  CORS response reflects the request's actual `Origin` header value — never
  `Access-Control-Allow-Origin: *`, which the CORS spec disallows for
  credentialed requests and which would be wrong here anyway since
  responses carry job titles/approval detail — and pairs it with
  `Access-Control-Allow-Credentials: true` so the browser attaches its
  cached Basic-auth credentials cross-origin. Because `Authorization` isn't
  a CORS-safelisted header, the browser sends a preflight `OPTIONS` first;
  that preflight is answered without calling `_authenticate()` (preflight
  requests never carry credentials, by design — nothing to authenticate
  yet) and returns `Allow-Methods: GET`/`Allow-Headers: Authorization`. The
  `<a target="_blank">` link is kept alongside the inline list as a
  fallback/"view all", and the inline fetch fails soft (leaves the list
  empty) on any network error, non-2xx status, or CORS failure — same
  "worst case is nothing shown" rule as the thumbnails.

### On-device diagnostics panel

The job-list page also has a collapsible diagnostics panel (`GET
/api/diag`, `WebUiServer::handleApiDiag()`) showing storage, battery, and
memory state — read-only, no new trust boundary (same session-cookie gate
as every other Web UI route). Every field follows the same "omit rather
than show a wrong or misleading value" rule used everywhere else in this
feature:

- **Storage.** `sd_total_bytes`/`sd_free_bytes` from `SDCardManager`'s
  `sdTotalBytes()`/`sdUsedBytes()` (subtracted here; clamped to 0 rather
  than underflowing if used ever exceeds total).
- **Battery.** `BatteryMonitor::readStatus()` (FreeInk SDK) reports
  `battery_percent`/`battery_millivolts`, each included only when that
  reading's own `percentageKnown`/`millivoltsKnown` flag is true.
  Charging status is deliberately never surfaced: X4's `BoardConfig`
  profile has no charge-status pin wired (`batteryChargeStatus =
  PIN_UNASSIGNED`), so `chargingKnown` would always read false — showing
  it would look like "definitely not charging" instead of "unknown."
- **Memory.** `heap_free_bytes` from `freeink::MemoryManager::instance().freeBytes()`
  — always present (no hardware-dependent unknown case here).

`joblist.html` hides each row individually when its backing field is
absent from the response, rather than showing a placeholder — a device
built without `BatteryMonitor` wired up, for instance, just shows Storage
and Free memory with no Battery row, not a broken or zeroed one.

## Memory budget (ESP32-C3, firmware)

The C3 has 400KB SRAM total, shared between the Wi-Fi/TLS stack, FreeRTOS,
the FreeInk display framebuffer, and application code. Concretely:

| Consumer | Bound | Why |
|---|---|---|
| Display framebuffer | 48,000 B (800x480 / 8, single-buffer mode, `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1`) | Fixed, owned by FreeInk |
| XTC page render | 2,048 B chunk buffer | `XtcReader` streams file→framebuffer in fixed chunks, §`docs/xtc-format.md` |
| SD download | 2,048 B chunk buffer | `SyncClient::downloadJobToSd()` streams HTTP→SD in fixed chunks; SHA-256 state is ~200 B, not proportional to file size |
| Job/outbox index | Bounded by `MAX_INBOX_JOBS` (64) and `MAX_OUTBOX_ENTRIES` (32) fixed-capacity JSON arrays, loaded once at boot (~6KB typical, up from ~4KB before the landscape-strip variant fields added roughly 120 bytes/`JobEntry`) | `JobStore`/`ApprovalOutbox` refuse to grow past these caps; the UI surfaces "inbox full, archive or delete something" rather than allocating unbounded state |
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
