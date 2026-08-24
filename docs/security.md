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
