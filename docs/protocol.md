# Sync & Relay Protocol Reference

This document is the wire-level contract between the three components:

- **X4 firmware** (`firmware/`) — the client that wakes, syncs, and sleeps.
- **Pi print server** (`pi-server/`) — the always-on home server, source of truth.
- **Cloud relay** (`relay/`) — a dumb store-and-forward for approvals when the X4
  is away from home. It never sees document bytes.

All identifiers are lowercase hex UUIDs (`uuid4().hex`, 32 chars, no dashes)
unless noted. All timestamps are Unix seconds (UTC, integer). All request/response
bodies are JSON (`application/json`) except file downloads.

## 1. Pi Sync API (X4 <-> Pi, direct on LAN)

Base URL: `https://<pi-host>:8443/api/v1` (self-signed cert generated at
install time; the X4 trusts it via a CA cert copied to `/system/pi_ca.pem` on
the SD card during pairing — see `docs/setup-x4.md`).

Every request carries:

```
Authorization: Bearer <device_token>
X-Device-Id: <device_id>
```

`device_token` and `device_id` are issued once by `pi-server/tools/pair_device.py`
and stored in `/system/device.json` on the X4's SD card.

### 1.1 `GET /devices/{device_id}/jobs?status=pending`

Returns manifests for jobs the device has not yet downloaded (or that changed
since the device's last ack — see `If-None-Match` note below).

```json
{
  "jobs": [
    {
      "job_id": "3f9a...e21",
      "title": "Invoice #4412",
      "created_at": 1737590000,
      "xtc_bytes": 184320,
      "xtc_sha256": "b5b2...9c",
      "page_count": 3,
      "status": "pending",
      "landscape_xtc_bytes": 219648,
      "landscape_xtc_sha256": "a1c4...02",
      "landscape_page_count": 5
    }
  ],
  "server_time": 1737590500
}
```

`status` query defaults to `pending` (jobs not yet acked by this device).
`status=all` returns every job still retained on the Pi regardless of ack
state, for recovery/debugging.

The three `landscape_*` fields are **optional** — present only when this
job has a landscape-strip rendering (see §4's "Landscape-strip variant"
note). Absent means none exists (never converted before this feature, or
the landscape conversion itself failed on this document's page shape); a
device that sees them absent uses only the normal variant, exactly as
before this field existed.

### 1.2 `GET /jobs/{job_id}/xtc?variant=<normal|landscape>`

Streams the XTC/XTCH file body. `variant` defaults to `normal`;
`variant=landscape` streams the landscape-strip rendering instead (404 if
the job has none). Supports HTTP `Range` requests (byte ranges) so the
firmware can resume an interrupted download without re-fetching bytes
already written to SD. Response headers:

```
Content-Type: application/x-xtc
Content-Length: <bytes>
X-Content-SHA256: <hex sha256 of the full file>
Accept-Ranges: bytes
```

The firmware computes SHA-256 incrementally while streaming to SD and MUST
compare it against `X-Content-SHA256` before marking the job downloaded. A
mismatch discards the partial file and retries (see §4 of `architecture.md`).

### 1.3 `POST /jobs/{job_id}/ack`

Body: `{"device_id": "...", "sha256": "<hex sha256 the device computed>", "landscape_sha256": "<optional, if the device also downloaded that variant>"}`

The server verifies `sha256` matches the stored normal-variant hash. When
the job has a landscape variant and the body includes `landscape_sha256`,
that is verified too — one ack still marks the whole job delivered, there
is no separate per-variant delivery state. `landscape_sha256` absent from
the body (a job with no landscape variant, or a device that hasn't
downloaded one) skips that check entirely, never a mismatch on its own. On
any hash match failure the server returns `409 Conflict` with
`{"status": "hash_mismatch"}` and the job stays `pending` for retry; on
full match it records the job as delivered to this device (so it drops out
of future `status=pending` listings) and returns `{"status": "ok"}`.

### 1.4 `POST /approvals`

Body:

```json
{
  "approval_id": "8c1e...f0",
  "device_id": "dev-...",
  "job_id": "3f9a...e21",
  "action": "print",
  "created_at": 1737591200
}
```

`action` is one of `print`, `keep`, `delete`. `approval_id` is generated
**on the device** the moment the user makes the choice (before it is even
persisted to the offline outbox) and never changes across retries — this is
the idempotency key. Every layer that touches an approval (Pi's local API,
the relay, and the CUPS-forward step) treats `approval_id` as a unique key:

- The Pi's `approvals` table has a `UNIQUE(approval_id)` constraint.
- Re-submitting the same `approval_id` (device retried after a dropped
  connection, or the same approval arrived via the relay *and* later
  directly once the device got home) is a no-op: the server looks up the
  existing row and returns the original result instead of reprocessing.

Response:

```json
{"approval_id": "8c1e...f0", "status": "applied", "detail": "printed", "cups_job_id": 42}
```

`status` is `applied` (first time processed), `already_applied` (dedup hit
— body still describes the original outcome), or `superseded` (see below).
For `action=print`, `detail` is one of `printed`, `print_failed` (with
`error` populated); for `keep` / `delete` it is `kept` / `archived`.

`superseded` covers a gap `approval_id`-based dedup alone doesn't:
`record_approval_if_new` only catches *retries of the same approval_id*
(the case above), not two *different* devices independently submitting
their own approval for the same `job_id` — e.g. two paired X4s that both
downloaded the same job before either had synced, and both tapped Print.
Each generates its own genuinely-new `approval_id`, so the Pi additionally
claims each still-undecided job's print outcome for whichever device-
originated `action=print` approval reaches it first
(`printer_forward.claim_job_for_finalization`); a second, different
`approval_id` for the same job gets `status=superseded` (`detail`
`superseded`, `error` naming the winning `approval_id`) instead of a
second `lp` invocation. Scoped to `action=print` from a device only —
`keep`/`delete` have no irreversible side effect to protect, and the
admin console's own reprint/re-decide actions (`received_via=admin`) are
never superseded. A device receiving `superseded` should treat it exactly
like `applied` for outbox purposes: this job's fate is settled, just not
by this approval — see §3.

### 1.5 `GET /devices/{device_id}/status`

Lightweight health/clock-sync check used before a full sync (cheap way to
confirm the Pi is reachable on the current network before attempting the
heavier job listing). Returns `{"server_time": 1737590500, "printer_ready": true}`.

### 1.6 `GET /devices/{device_id}/config`

Household-wide (not per-device) calendar feeds and Wi-Fi networks, managed
from the Pi's admin console (`admin_api.py`'s "Calendars & Wi-Fi" tab) and
pulled by every paired device on each sync — see
`firmware/src/sync/SyncManager.cpp`. This is the primary way to manage
both lists once a device is paired; hand-editing `/system/calendars.json`
and `/system/wifi.json` on the SD card (`docs/setup-x4.md`) still works
and remains the *only* way to get a brand-new device onto Wi-Fi for its
very first sync (a device with no saved network yet has no way to reach
this endpoint in the first place).

```json
{
  "calendars": [{"url": "https://calendar.google.com/calendar/ical/.../basic.ics", "label": "Work"}],
  "wifi_networks": [{"ssid": "HomeWiFi", "password": "hunter2"}],
  "server_time": 1737590500
}
```

The device applies these two lists differently, both on the firmware side
(`config::CalendarConfig`/`config::WifiStore`):

- **Calendars**: wholesale replace — the Pi's list becomes the device's
  entire `/system/calendars.json` contents. There's no on-device way to
  add a calendar independently of the Pi, so nothing is lost by this.
- **Wi-Fi networks**: merged in via `WifiStore::addOrUpdate()` (insert or
  update by SSID) — **never** a wholesale replace, and never a delete. A
  device is only ever reading this endpoint because it's already
  successfully connected to *some* saved network; if the Pi's list
  happened to omit that network (e.g. an admin only entered a guest SSID),
  a wholesale replace would erase the very credential the device used to
  get here, stranding it. Merge-only means the Pi's list can only ever
  grow what the device knows, never shrink it — removing a network
  on-device (Settings > Wi-Fi tab, view + remove) is the only way to
  actually forget one, and only sticks if the Pi's list doesn't still
  include it (removed there too, or never added).

Both lists are capped by firmware's fixed-capacity arrays
(`config::kMaxCalendars` = 4, `config::kMaxWifiNetworks` = 8) — the admin
console's add endpoints reject a request that would exceed either cap
(`admin_api.py`'s `MAX_CALENDAR_FEEDS`/`MAX_WIFI_NETWORKS`) rather than
letting the device silently truncate the list on sync.

### 1.7 `POST /devices/{device_id}/jobs/{job_id}?title=<title>`

The direct-upload endpoint behind the on-device web UI's "Upload" button
(`ui/WebUiServer.cpp`, `ui/pages/joblist.html`) — lets a phone create a
real print job straight on the X4, with no Pi involved at the moment of
upload, and later hand it off for real printing once the Pi is reachable.
See `docs/architecture.md`'s direct-upload section for the full flow; this
endpoint is step 3 of it.

Request body is the raw image bytes (not JSON) — `Content-Type` is
`image/jpeg` or `image/png` (any other value is rejected; this endpoint
intentionally accepts a narrower set than the IPP path's
`SUPPORTED_MIME_TYPES`, since the on-device WASM converter that feeds it
only ever produces/forwards these two). `job_id` in the path is generated
**on the device**, the same idempotency-key philosophy `approval_id`
already uses (§1.4): it's the id the device already created a
`JobEntry`/queued a Print approval against locally, so the row this
creates lines up with what the device's `ApprovalOutbox` expects to sync
next.

Response:

```json
{"job_id": "3f9a...e21", "status": "created"}
```

`status` is `created` (first time this `job_id` was seen) or
`already_exists` (retried after a dropped connection — a cheap no-op, not
a duplicate row or a repeated conversion pass; see
`convert.ingest_document`). Once this returns successfully, the job
behaves exactly like an IPP-submitted one for every purpose downstream
(listed to devices, downloadable, printable) — its only difference is
`jobs.source = "x4_upload"` instead of `"ipp"`.

### 1.8 Planner & Pomodoro API

See `docs/planner.md` for the feature this backs (color-coded/icon-based
timeline + Pomodoro, `pi-server/focusink_server/planner.py`). Tasks are
authored on the Pi (admin console) and pulled by the device on its normal
sync pass, the same "authored centrally, pushed to the device" shape §1.6
uses for calendars/Wi-Fi — except per-device and per-day rather than
household-wide.

#### `GET /devices/{device_id}/planner/tasks?date=<YYYY-MM-DD>`

```json
{
  "tasks": [
    {"id": 1, "title": "Standup", "category": "Work", "start_time": "09:00", "end_time": "09:15", "done": false}
  ],
  "server_time": 1737590500
}
```

`category` is one of `Work`, `Break`, `Chore`, `Health`, `Social`,
`School`, `Personal`, `Other` (`planner.CATEGORIES`) — the fixed set the
firmware's `store::Category` enum and the on-device web UI's
`planner.html` both key off of; do not reorder or extend without updating
all three. `date` is required; a missing or malformed value is a `400`.
Response is wrapped `{"tasks": [...], "server_time": ...}` to match §1.1's
job-list shape, unlike §1.9 below.

#### `POST /devices/{device_id}/planner/tasks/{task_id}/complete`

Body:

```json
{"completion_id": "c1a2...f0"}
```

`completion_id` is generated **on the device**, the same idempotency-key
philosophy `approval_id` uses (§1.4) — a client-generated key created
before the sync attempt, so a retried POST (dropped connection, device
reboot mid-sync) is a no-op rather than a duplicate. Unlike an approval,
completing a task has no external side effect (no `lp` call) — just
flipping `planner_tasks.done` — so the Pi records and applies it in one
SQLite transaction (`Database.complete_task_if_new`) rather than
`apply_approval`'s record → external-effect → mark-applied split.

Response:

```json
{"completion_id": "c1a2...f0", "task_id": 1, "status": "applied", "done": true}
```

`status` is `applied` (first time this `completion_id` was seen) or
`already_applied` (dedup hit — `done` still reflects the real, unchanged
state). A `task_id` that doesn't exist, or belongs to a different device,
is a `404`.

### 1.9 `GET /devices/{device_id}/pomodoro/config`

Per-device Pomodoro durations, defaulting to
`planner.DEFAULT_POMODORO_CONFIG` when a device has never had its own
config set (no server-side provisioning step needed at pairing time).
Response is the bare config dict — no `{"...": ..., "server_time": ...}`
wrapper, unlike every other GET in this section, since the contract here
*is* the full literal shape:

```json
{
  "work_minutes": 25,
  "break_minutes": 5,
  "long_break_minutes": 15,
  "sessions_before_long_break": 4,
  "checkpoint_minutes": 5
}
```

`checkpoint_minutes` is how often the device's RTC timer wakes it to
redraw remaining time during an active session — see `docs/planner.md`
for why this is a checkpoint cadence, not a live tick.

## 2. Relay Protocol (X4 <-> Relay <-> Pi, over the internet)

Base URL: `https://<relay-host>/relay/v1`. Both the Pi and the X4 authenticate
with the same **account token** issued at pairing time (`pair_device.py`
registers the account with the relay and prints the token alongside the
device pairing file). The relay only ever stores: account id, device id,
approval envelopes (same shape as §1.4, no document bytes), and delivery
state.

### 2.1 `POST /accounts/{account_id}/approvals` (X4 -> relay)

Same body as §1.4 plus the bearer account token. The relay stores the
envelope keyed by `approval_id` (idempotent insert) with `delivered=false`
and returns `{"approval_id": "...", "status": "queued"}` immediately — it
does not wait for the Pi.

### 2.2 `GET /accounts/{account_id}/approvals/pending` (Pi -> relay, polled)

The Pi polls this every `RELAY_POLL_INTERVAL_SECONDS` (default 20s, only
while the relay client is enabled) using its outbound-only HTTPS connection.
Returns undelivered approval envelopes:

```json
{"approvals": [{"approval_id": "...", "device_id": "...", "job_id": "...", "action": "print", "created_at": 1737591200}]}
```

### 2.3 `POST /accounts/{account_id}/approvals/{approval_id}/ack` (Pi -> relay)

Sent after the Pi has applied the approval locally (via the exact same
idempotent `approvals` table as the direct path in §1.4 — the relay path and
the LAN path converge on one code path in `pi-server`). Marks the envelope
`delivered=true` so it stops appearing in §2.2 and can be pruned after a
retention window (default 14 days).

### 2.4 `GET /accounts/{account_id}/approvals/{approval_id}` (X4 -> relay, optional)

The X4 may poll this the next time it has any connectivity (home Wi-Fi or
relay) to confirm an approval it submitted while away was actually delivered
and applied, so it can mark the local outbox entry `synced` and stop
retrying it. Returns the same `{"status": "applied"|"already_applied"|"pending"}`
shape.

### 2.5 Optional remote XTC sync

Disabled by default (`relay.allow_document_sync = false` in `pi-server`
config). When explicitly enabled, the Pi additionally uploads pending job
*manifests* (never content, still) to `POST /accounts/{account_id}/jobs`,
and the X4 can fetch the XTC bytes through a relay-proxied
`GET /accounts/{account_id}/jobs/{job_id}/xtc` while away from home. This
path is opt-in and documented separately in `docs/relay.md` because it moves
document bytes off the home network — the default posture keeps originals
and previews on the LAN only.

## 3. Idempotency summary

| Layer | Key | Effect of a duplicate |
|---|---|---|
| Pi `approvals` table | `approval_id` (UNIQUE) | Returns the stored result; never re-submits to CUPS |
| Pi `jobs.finalizing_approval_id` (§1.4) | First device-originated `action=print` approval_id to claim a given `job_id` | A *different* approval_id for the same job (two devices racing) gets `status=superseded` instead of a second CUPS submission — closes the gap approval_id-only dedup above doesn't cover |
| CUPS submission | `approvals.cups_job_id` set only on first success | A crash between "insert approval row" and "submit to CUPS" is safe: on restart the Pi finds rows with `applied=0` and (re)submits exactly once, because the submission itself happens inside the same transaction boundary as marking `applied=1` |
| Relay `approvals` table | `approval_id` (UNIQUE) | Duplicate POST from a flaky retry just returns `queued` again, no duplicate delivery to the Pi |
| Device outbox | `approval_id` generated once, stored durably before any network attempt | Reboots/power loss mid-sync never lose or duplicate an approval — the outbox entry is retried until acked, using the same id |
| Pi `jobs` table (§1.7) | `job_id` (PRIMARY KEY), caller-supplied by the direct-upload endpoint | A retried upload (dropped connection after the Pi already ingested it) is a no-op (`status=already_exists`), not a duplicate row or conversion pass |
| Pi `task_completions` table (§1.8) | `completion_id` (PRIMARY KEY), device-generated | A retried completion POST is a no-op (`status=already_applied`); `planner_tasks.done` is only ever flipped once, inside the same transaction as the ledger insert |

## 4. XTC/XTG file format

The X4 reads the exact upstream format documented by
[phrozen/xtx SPEC.md](https://github.com/phrozen/xtx/blob/main/SPEC.md) — see
`docs/xtc-format.md` for the summary this project relies on and the specific
subset `pi-server/focusink_server/xtc_writer.py` emits (monochrome XTG
pages only; XTCH/grayscale is read-compatible in firmware but not produced by
the converter, see that doc for the rationale).

**Landscape-strip variant**: the Pi generates a second XTC file per job
(best-effort — see `xtc_writer.prepare_landscape_strip_images`) where each
source page is split into panel-sized, pre-rotated strips meant to be read
with the device turned 90°, using the panel's full width as reading length
instead of being bound by its shorter dimension. This needed **no format
change** — it is still an ordinary XTC container, just with more pages per
source document, each already sized and rotated to exactly the panel's
native resolution so the firmware's existing raw-copy render path handles
it unchanged (see §1.1/§1.2/§1.3 above for how a device discovers,
downloads, and acks this second file).
