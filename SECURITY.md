# Security Policy

## Reporting a vulnerability

This is a home/personal-scale project without a dedicated security contact.
To report a vulnerability:

- Preferred: open a [GitHub private security advisory](../../security/advisories/new)
  on this repository — it's visible only to maintainers until you and they
  agree to disclose.
- If that's not workable, open a regular issue. Avoid including exploit
  details or anything that would expose a real deployment (IPs, hostnames,
  tokens) in a public issue — describe the class of problem and a
  maintainer will follow up for specifics privately.

There's no fixed SLA, but reports will be acknowledged and triaged as soon
as a maintainer sees them.

## Trust boundaries

Full detail, including what's explicitly out of scope for this prototype,
lives in [`docs/security.md`](docs/security.md). Summary:

- **Pi sync API** (`sync_api.py`): TLS (self-signed cert, pinned by the X4
  at pairing time) + a per-device bearer token. LAN-only by design — don't
  port-forward it to the internet.
- **Relay**: carries only device/job IDs, actions, and timestamps — never
  document bytes. It cannot cause a print on its own: the Pi independently
  re-validates any approval it relays (device token check) before ever
  touching CUPS.
- **CUPS forwarding** (`printer_forward.py`): always prints to a single,
  fixed, pre-configured queue set at install time — never a queue name or
  argument derived from request data.

The IPP listener itself is intentionally **unauthenticated**, matching how
every consumer network printer behaves — see `docs/security.md` for the
reasoning and the mitigation (network segmentation) if that doesn't fit
your threat model.
