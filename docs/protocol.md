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
      "status": "pending"
    }
  ],
  "server_time": 1737590500
}
```

`status` query defaults to `pending` (jobs not yet acked by this device).
`status=all` returns every job still retained on the Pi regardless of ack
state, for recovery/debugging.

### 1.2 `GET /jobs/{job_id}/xtc`

Streams the XTC/XTCH file body. Supports HTTP `Range` requests (byte ranges)
so the firmware can resume an interrupted download without re-fetching bytes
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

Body: `{"device_id": "...", "sha256": "<hex sha256 the device computed>"}`

The server verifies the hash matches the stored XTC file's hash. On match it
records the job as delivered to this device (so it drops out of future
`status=pending` listings) and returns `{"status": "ok"}`. On mismatch it
returns `409 Conflict` with `{"status": "hash_mismatch"}` and the job stays
`pending` for retry.

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

`status` is `applied` (first time processed) or `already_applied` (dedup hit
— body still describes the original outcome). For `action=print`, `detail`
is one of `printed`, `print_failed` (with `error` populated); for `keep` /
`delete` it is `kept` / `archived`.

### 1.5 `GET /devices/{device_id}/status`

Lightweight health/clock-sync check used before a full sync (cheap way to
confirm the Pi is reachable on the current network before attempting the
heavier job listing). Returns `{"server_time": 1737590500, "printer_ready": true}`.

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
| CUPS submission | `approvals.cups_job_id` set only on first success | A crash between "insert approval row" and "submit to CUPS" is safe: on restart the Pi finds rows with `applied=0` and (re)submits exactly once, because the submission itself happens inside the same transaction boundary as marking `applied=1` |
| Relay `approvals` table | `approval_id` (UNIQUE) | Duplicate POST from a flaky retry just returns `queued` again, no duplicate delivery to the Pi |
| Device outbox | `approval_id` generated once, stored durably before any network attempt | Reboots/power loss mid-sync never lose or duplicate an approval — the outbox entry is retried until acked, using the same id |

## 4. XTC/XTG file format

The X4 reads the exact upstream format documented by
[phrozen/xtx SPEC.md](https://github.com/phrozen/xtx/blob/main/SPEC.md) — see
`docs/xtc-format.md` for the summary this project relies on and the specific
subset `pi-server/xteink_print_server/xtc_writer.py` emits (monochrome XTG
pages only; XTCH/grayscale is read-compatible in firmware but not produced by
the converter, see that doc for the rationale).
