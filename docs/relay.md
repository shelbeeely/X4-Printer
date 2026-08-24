# Cloud Relay

The relay (`relay/`) lets you approve a queued print from your phone's
hotspot at the airport, not just from your home Wi-Fi. It is a small,
self-hosted, pure-stdlib-Python service — no framework, no database server,
just SQLite and `http.server` — designed to be trivial to audit and cheap to
run (a $5/month VPS, or a free-tier instance, is enough).

## What it does and does not do

- It stores **approval envelopes**: `{approval_id, device_id, job_id,
  action, created_at}` — never document bytes, never the original PDF,
  never the XTC preview (unless you explicitly opt into §"Optional remote
  XTC sync" below).
- The **Pi makes the only outbound connection that matters for delivery**:
  it polls `GET /accounts/{id}/approvals/pending` every
  `XTEINK_RELAY_POLL_INTERVAL` seconds (default 20s). Nothing is ever
  pushed *to* the Pi, and no port is opened on your home router/NAT.
- The **X4 also only makes outbound connections**: when it's away from home
  and the user approves a document, `POST /accounts/{id}/approvals` queues
  the envelope. The X4 never needs to be reachable from the internet.
- The relay **cannot cause a print by itself**. It can only relay an
  approval envelope; the Pi independently re-validates it (the job must
  exist in the Pi's own database, the action must be one of
  `print`/`keep`/`delete`) before ever touching CUPS. A compromised relay
  can, at worst, replay approvals for jobs that already exist on your Pi —
  and even then, `approval_id`-based idempotency (docs/protocol.md §3)
  means a replayed approval either matches one you already made (no-op) or
  is a brand-new `approval_id` the relay itself can't forge without your
  device's token, which the relay is never given (the *account* token
  authenticates to the relay; the device's separate token to the Pi is
  never sent to the relay).

## Setting it up

```sh
cd relay
sudo ./install/install.sh
sudo -u xteink-relay /opt/xteink-relay/.venv/bin/python \
  /opt/xteink-relay/tools/create_account.py --name "My Household"
```

This prints an `account_id` and `account_token`. Put both, plus the relay's
URL, into the Pi's systemd unit (`pi-server/install/xteink-print-server.service`,
or `sudo systemctl edit xteink-print-server.service`):

```
[Service]
Environment=XTEINK_RELAY_URL=https://relay.example.com:8843
Environment=XTEINK_RELAY_ACCOUNT_ID=acct-...
Environment=XTEINK_RELAY_ACCOUNT_TOKEN=...
```

Then re-run `pair_device.py` for each X4 (or re-copy an updated
`device.json` — the relay fields are only added when those three env vars
are already set on the Pi at pairing time).

## Deployment options / TLS

The relay process can terminate TLS itself
(`XTEINK_RELAY_TLS_CERT`/`XTEINK_RELAY_TLS_KEY`, e.g. from `certbot`), or —
recommended — sit behind a standard reverse proxy (nginx, Caddy, or your
VPS provider's load balancer) that terminates TLS on 443 and forwards
plaintext to `127.0.0.1:8843`. Either way, **never** run the relay's own
port open to the internet without TLS: approval envelopes are low-sensitivity
(no document content) but the account token is a real credential.

## Multi-tenancy

This prototype's `create_account.py` is an operator-run CLI, matching the
"one person self-hosts a relay for their own household(s)" use case the
task describes. Turning it into a public multi-tenant service (self-serve
signup, per-account rate limits, billing) is a natural extension but out of
scope here — the account/token model in `relay_server/db.py` and `app.py`
already gives you tenant isolation (see `tests/test_relay.py::test_cross_account_isolation`);
what's missing for a public service is signup UX and abuse controls, not
architecture.

## Optional remote XTC synchronization

Disabled by default. The task allows the X4 to "synchronize through the
relay when away from home if practical" — this prototype implements the
*approval* half of that unconditionally (§ above) because it needs no
document bytes to leave the home network. Syncing new *documents* (XTC
previews) through the relay while away is a materially different trust
decision — it means print-inbox content leaves your LAN — so it is gated
behind an explicit opt-in:

```
Environment=XTEINK_RELAY_ALLOW_DOCUMENT_SYNC=true
```

This prototype does not implement the proxy endpoints for that path (the
`pi-server` and `relay` route tables have no `/jobs`/`/jobs/{id}/xtc`
routes under `/relay/v1/accounts/{id}/...`) — the flag exists as the
documented extension point and the architectural decision point
(docs/architecture.md's security model table), so a deployment that needs
it has one clearly-scoped place to add it rather than a surprise data path
threaded through existing code. If you build it: keep the same
`approval_id`-style idempotency, add a size/TTL cap on the relay's
temporary storage (documents should never live on the relay longer than a
single retrieval), and document the changed trust boundary in
`docs/security.md` before turning it on for a real deployment.
