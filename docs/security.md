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
```

## IPP listener (`pi-server/xteink_print_server/ipp_server.py`)

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

## Admin web console (`admin_api.py`)

- **Disabled by default.** `server.py` only starts the listener if
  `XTEINK_ADMIN_PASSWORD` is set — an empty password means no listener at
  all, not an open one. There's no separate "enable" flag to forget.
- **Auth is a single shared HTTP Basic password**, compared with the same
  constant-time comparison (`util.constant_time_eq`) the sync API uses for
  bearer tokens — not per-user accounts, matching the project's existing
  one-secret-per-surface simplicity. The password itself is never hashed
  or persisted anywhere — it only ever lives in the process environment
  (`admin_settings.json` is not used for it; only the fields listed in
  `config.RUNTIME_OVERRIDABLE_FIELDS` are), so anyone who can read the
  systemd unit or process environment can read it — same exposure as
  `XTEINK_RELAY_ACCOUNT_TOKEN` today.
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
  revoke or rotate any device's token, and edit `cups_queue`,
  `retention_days`, and the relay fields live. This is meaningfully more
  powerful than the sync API (which only ever acts on the one device
  presenting a valid token for itself) — treat the admin password with at
  least the same care as the relay account token, and don't port-forward
  this any more than you would the sync API.
- **No rate limiting or lockout** on repeated failed Basic-auth attempts —
  same "no rate limiting" gap already documented below for the other
  listeners, not something this feature adds beyond what already exists.

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
(`XTEINK_CUPS_QUEUE`), never from request data, so no approval payload can
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
