# Security Model

This is a prototype for a home/personal-scale deployment, not a hardened
multi-tenant product. This document is honest about what is and isn't
covered so a deployer can make an informed call about their own threat
model.

## Trust boundaries

```
[Any device on the LAN] --IPP, no auth--> [Pi: ipp_server.py]
[Paired X4]  --HTTPS, device bearer token, pinned self-signed CA--> [Pi: sync_api.py]
[Pi]         --HTTPS, account bearer token, outbound only-->        [Relay]
[X4, away]   --HTTPS, account bearer token, outbound only-->        [Relay]
[Pi]         --local subprocess, fixed queue name-->                 [CUPS -> physical printer]
[Admin browser, LAN] --HTTP Basic, shared password, opt. TLS--> [Pi: admin_api.py]
[Phone, LAN or X4's own hotspot] --HTTP, fresh PIN + session cookie, no TLS--> [X4: ui/WebUiServer.cpp]
```

## IPP listener (`pi-server/focusink_server/ipp_server.py`)

**Unauthenticated by design**, matching every consumer/home network
printer (real hardware printers with IPP/AirPrint accept jobs from anyone
on the LAN too). Do not port-forward this to the internet. If your threat
model includes untrusted devices on the same LAN (a shared/guest network,
an office), put the Pi on a separate VLAN/segment from those devices, the
same mitigation you'd apply to a physical printer.

## X4 sync API (`sync_api.py`)

- TLS via a self-signed certificate generated at install time
  (`tools/gen_selfsigned_cert.py`), pinned by the X4 (copied to its SD card
  as `/system/pi_ca.pem` at pairing time) rather than trusted via a public
  CA — appropriate for a LAN-only endpoint with no DNS name, and it means
  the X4 will *refuse* to talk to an impostor server even on a compromised
  network (no CA-trust fallback for this endpoint — see
  `SyncClient::configureClientForEndpoint` in
  `firmware/src/net/SyncClient.cpp`, which returns failure rather than
  falling back to insecure when no pinned cert is loaded).
- Per-device bearer token (`tools/pair_device.py`), stored hashed
  (SHA-256) on the Pi (`devices.token_hash`), never in plaintext server-side.
  The token itself lives in plaintext in the device's own `device.json`
  once paired — appropriate given it only grants access to that one
  device's own jobs/approvals, and the file lives on a physical SD card in
  a physical device the same person owns.
- Regenerating the Pi's certificate (e.g. after a hostname/IP change)
  requires re-copying it to every paired device — there's no revocation
  list or rotation flow in this prototype. A production deployment would
  want the Pi to serve a stable CA it can rotate leaf certs under, or a
  real DNS name + Let's Encrypt.

## On-device Web UI (`firmware/src/ui/WebUiServer.h`/`.cpp`)

This is the one place in the whole system that departs from "the X4 never
accepts inbound connections" (`docs/architecture.md`'s wake sequence) —
deliberately, and only for as long as a person has just turned it on from
the physical UI.

- **Off unless explicitly toggled on**, and torn down by the same idle
  timer that governs the rest of the on-device UI
  (`main.cpp`'s `kIdleSleepTimeoutMs`) — there is no way for this listener
  to stay up across a deep sleep cycle; `goToSleep()` stops it
  unconditionally before every sleep.
- **A fresh 6-digit PIN, shown on the e-ink screen, gates every session.**
  This is meaningfully weaker than the Pi admin console's password (a
  6-digit PIN is 1,000,000 possibilities, not attacker-chosen-length) and
  is **not rate-limited** — a scripted attacker already on the same
  network (home Wi-Fi mode) or already holding the hotspot password
  (hotspot mode) could brute-force it in a reasonably short time. This is
  an accepted, documented tradeoff for a device with no keyboard and a
  tiny e-ink screen, not an oversight — the actual gate in hotspot mode is
  really the WPA2 password (also freshly generated, shown on-screen,
  never persisted), with the PIN as a second, weaker layer on top.
- **Plain HTTP, no TLS.** Same reasoning as the IPP listener: adding a
  self-signed cert story to an ephemeral, manually-toggled, single-client
  embedded listener is disproportionate. Traffic (job titles, print/keep/
  delete actions) is visible to anyone who could already reach the
  listener under the point above.
- **Session token is the real credential**, not the PIN itself: `/login`
  only ever checks the PIN once and then issues a 128-bit random
  `esp_random()`-backed cookie; that token, not the PIN, is what every
  subsequent request actually needs to present. The PIN's weakness above
  is about *getting* a valid session, not about the session mechanism
  itself.
- **Actions reuse the Pi-side idempotency story too**: every approval
  queued from the phone goes through `store::enqueueApproval()`
  (`firmware/src/store/ApprovalOutbox.h`), the exact same durable-outbox
  path the physical buttons use, so it inherits the same
  `docs/protocol.md` §3 guarantees once it eventually syncs to the Pi.

## Admin web console (`admin_api.py`)

- **Disabled by default.** `server.py` only starts the listener if
  `FOCUSINK_ADMIN_PASSWORD` is set — an empty password means no listener at
  all, not an open one. There's no separate "enable" flag to forget.
- **Auth is a single shared HTTP Basic password**, compared with the same
  constant-time comparison (`util.constant_time_eq`) the sync API uses for
  bearer tokens — not per-user accounts, matching the project's existing
  one-secret-per-surface simplicity. The password itself is never hashed
  or persisted anywhere — it only ever lives in the process environment
  (`admin_settings.json` is not used for it; only the fields listed in
  `config.RUNTIME_OVERRIDABLE_FIELDS` are), so anyone who can read the
  systemd unit or process environment can read it — same exposure as
  `FOCUSINK_RELAY_ACCOUNT_TOKEN` today.
- **TLS is reused, not separate**: if `tls_cert`/`tls_key` exist (the same
  cert `sync_api.py` uses), the admin listener wraps its socket in TLS
  too; otherwise it logs a warning and serves plaintext, same fallback
  behavior as the sync API.
- **Live-edited relay settings persist to `<data_dir>/admin_settings.json`**
  (`config.save_runtime_overrides`), which can contain
  `relay_account_token` in plaintext — that file is written `0600` (owner
  only), same intent as the TLS private key
  (`tools/gen_selfsigned_cert.py`).
- **What a logged-in admin can do**: see every job's title/size/status and
  every paired device's name, reprint/keep/archive/requeue/purge any job,
  revoke or rotate any device's token, edit `cups_queue`,
  `retention_days`, and the relay fields live, and — as of the on-device
  Web UI's full-document preview feature and its later thumbnail/recent-
  activity extensions — download any job's untouched original document
  (`GET .../jobs/{id}/original`), its thumbnail (`GET
  .../jobs/{id}/thumbnail`), and any device's approval history (`GET
  .../devices/{id}/approvals`). This is meaningfully more powerful than
  the sync API (which only ever acts on the one device presenting a valid
  token for itself) — treat the admin password with at least the same
  care as the relay account token, and don't port-forward this any more
  than you would the sync API.
- **All three of those routes are reachable from any device that knows
  the admin password**, not just the paired X4 — by design, since the
  whole point (see `docs/architecture.md` "On-device Web UI full-document
  preview") is that a phone's browser fetches them directly, bypassing the
  X4 entirely. None of them widen what the password already grants: a
  logged-in admin could already read `original_bytes`/`original_mime`
  metadata for every job via `GET .../jobs`, and every device's approval
  history via the existing unfiltered `GET .../approvals` — these routes
  just serve the same underlying data pre-filtered or as raw bytes instead
  of metadata.
- **No rate limiting or lockout** on repeated failed Basic-auth attempts —
  same "no rate limiting" gap already documented below for the other
  listeners, not something this feature adds beyond what already exists.
- **`GET .../devices/{id}/approvals` is CORS-enabled**, uniquely among
  these routes — the X4 page (`joblist.html`) fetches it directly, inline,
  from its own origin (a different host/port than the Pi), which browsers
  only allow with the right CORS response. That response reflects the
  request's actual `Origin` header (never `Access-Control-Allow-Origin:
  *`, which is invalid for credentialed requests and would defeat the
  point of gating this behind the admin password) and sets
  `Access-Control-Allow-Credentials: true` so the browser attaches its
  cached Basic-auth credentials cross-origin; a preflight `OPTIONS`
  request (required because `Authorization` isn't a CORS-safelisted
  header) is answered the same way but without an auth check, since
  preflights never carry credentials. `original` and `thumbnail` stay
  CORS-free — they're loaded via `<a>`/`<img>`, which never trigger CORS
  enforcement in the first place, so there's nothing to add.

  Worth being precise about what this *does* change: any origin can send
  the preflight and get a reflected `Allow-Origin`, so the gate is still
  the admin password/cached Basic-auth, not the origin check — the same
  trust boundary `original`/`thumbnail` already rely on (a browser that
  has ever authenticated to the Pi attaches those cached credentials to
  any site's request for these URLs, CORS or not). What's new here is that
  a successful cross-origin `fetch()` lets a page's *script* read the JSON
  body, where an `<img>`/top-level `<a>` load only ever rendered pixels or
  required a visible, user-driven navigation. That is a real difference in
  exploitability if an admin's browser has cached credentials and later
  visits a hostile page — it does not, however, let anyone who lacks those
  cached credentials (or the password) reach this data, so it is not a new
  way to *obtain* access, only a wider blast radius for the same
  already-documented "any page in a browser holding cached admin
  credentials can hit these routes" exposure. Given this is a prototype for
  trusted home/personal-scale deployment (not a hardened multi-tenant
  product, per this doc's scope above), that tradeoff is accepted in
  exchange for the inline activity list; it's the same reasoning already
  written below for why these routes being reachable cross-origin isn't a
  privilege escalation on its own.

## Relay (see `docs/relay.md` for full detail)

- Carries only IDs/actions/timestamps by default — no document content,
  so a compromised relay cannot read what you're printing.
- A compromised relay **can**: see which devices exist and when approvals
  happen (metadata leakage), replay an approval it already saw (harmless —
  `approval_id` idempotency, see `docs/protocol.md` §3), and refuse/delay
  delivery (availability, not integrity). It **cannot** fabricate a new
  approval action, because it never has a device's Pi-side token (only the
  shared account token, which the Pi independently re-validates against
  its own device records before ever calling CUPS).
- The account token is shared across every device in a household by
  design (one relay account per Pi) — a compromised *device* can therefore
  submit approvals for any job on that household's Pi via the relay, same
  as it already can via the direct LAN path. This is not a privilege
  escalation; it's the same trust radius as physically having one of the
  household's X4s.

## CUPS forwarding (`printer_forward.py`)

`submit_to_cups()` only ever invokes `lp -d <config.cups_queue>` — the
queue name comes from server configuration
(`FOCUSINK_CUPS_QUEUE`), never from request data, so no approval payload can
redirect a print to an arbitrary destination or inject `lp` arguments (the
file path is passed as a single argv element via `subprocess.run([...])`,
never through a shell, so there is no shell-injection surface even though
job titles/filenames are user-controlled text from the original print job's
metadata).

## Known limitations (explicitly out of scope for this prototype)

- **No certificate rotation/revocation flow** (above).
- **No rate limiting** on the IPP listener, sync API, or relay — a
  malicious LAN device could flood the Pi with conversion work (PDF
  rendering is CPU-bound; PyMuPDF has its own hardening against malformed
  PDFs, but resource exhaustion via a very large legitimate-looking PDF is
  not specifically guarded against beyond `convert.py`'s `MAX_PAGE_COUNT`).
  The on-device Web UI's PIN has the same gap — see "On-device Web UI"
  above for why that one is a deliberately accepted tradeoff rather than
  an omission.
- **No per-account rate limiting on the relay** — see `docs/relay.md`
  "Multi-tenancy" for what a public deployment would need to add.
- **Wi-Fi credential storage is obfuscation, not encryption**
  (`firmware/src/config/WifiStore.h`), matching the reference project
  (CrossPoint Reader) it's modeled on and documented as such there too.
- **The relay's TLS defaults to `setInsecure()`** on the firmware side if
  no CA is provisioned at `/system/relay_ca.pem` (see
  `SyncClient::configureClientForEndpoint`) — provision that file for any
  real deployment; the fallback exists so a misconfigured relay CA fails
  soft (approvals just don't sync) rather than bricking the device's
  ability to build at all for want of a bundled root CA store. Production
  builds should provision the relay's CA (or the public root that issued
  it) unconditionally, not rely on the fallback.
