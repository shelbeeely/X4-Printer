---
name: run-relay
description: Build, run, and drive the X4 Print Inbox cloud relay (store-and-forward approval-envelope API). Use when asked to start the relay, exercise its approval endpoints, or smoke-test the relay end to end.
---

The relay is a real, pure-stdlib Python HTTP service (`python -m
relay_server.server`, `http.server` only — zero third-party dependencies
by design, see `requirements.txt`). It carries only approval envelopes
(device/job IDs, actions, timestamps), never document bytes. Drive it via
`driver.sh` in this skill directory: it launches a real instance, creates
a real account, and exercises the full submit → pending → ack lifecycle
with plain `curl` (no client library needed).

All paths below are relative to `relay/`.

## Prerequisites

None beyond system Python 3 — no venv, no pip install, nothing to build.

## Build

None.

## Run (agent path)

```bash
.claude/skills/run-relay/driver.sh smoke
```

One command: launches the relay in the background (plaintext, dev-only —
see Gotchas), creates a throwaway account via `tools/create_account.py`,
then submits a real approval envelope, lists it pending, acks it, confirms
it drops out of the pending list, and confirms an unauthenticated request
gets a 401. Leaves the relay running afterward — call `stop` when done.

For just launching a long-lived instance to poke at manually:

```bash
.claude/skills/run-relay/driver.sh start
# prints the base URL, account_id, and account_token
```

Stop it:

```bash
.claude/skills/run-relay/driver.sh stop
```

| command | what it does |
|---|---|
| `driver.sh start` | Launch in the background (idempotent), create an account if none exists yet, print connection info |
| `driver.sh smoke` | `start` + full submit/pending/ack/unauth-401 drive-through |
| `driver.sh stop` | Kill the background instance |

Once running, drive it directly with curl (account id/token are in
`/tmp/run-relay-scratch/account.env` by default — `source` it):

```bash
source /tmp/run-relay-scratch/account.env
BASE="http://127.0.0.1:18843/relay/v1/accounts/$ACCOUNT_ID"

curl -X POST "$BASE/approvals" -H "Authorization: Bearer $ACCOUNT_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"approval_id":"a1","device_id":"dev-1","job_id":"job-1","action":"print","created_at":1}'

curl "$BASE/approvals/pending" -H "Authorization: Bearer $ACCOUNT_TOKEN"
curl -X POST "$BASE/approvals/a1/ack" -H "Authorization: Bearer $ACCOUNT_TOKEN" -d '{}'
```

Override port/scratch dir via env vars before calling `driver.sh`:
`RUN_RELAY_PORT` (default `18843`), `RUN_RELAY_SCRATCH` (default
`/tmp/run-relay-scratch`).

## Run (human path)

```bash
python3 tools/create_account.py --name "My Household"   # once
python3 -m relay_server.server
```

Blocks in the foreground (real deployments run it under the systemd unit
in `install/`, behind a TLS-terminating reverse proxy). `Ctrl-C` to stop.

## Test

```bash
python3 -m pytest -q
```

5 tests, all pass.

## Gotchas

- **No TLS configured = plaintext by design**, with a loud warning logged
  at startup — the project's own docs say this is only acceptable behind a
  TLS-terminating reverse proxy on the same host, never exposed directly.
  `driver.sh` runs plaintext for simplicity; don't do that for anything
  but local testing.
- **A real bug, found by actually running this and fixed as part of
  building this skill**: `RelayConfig.tls_cert`/`tls_key` used to default
  to `Path(_env_str(name, ""))`. `Path("")` normalizes to `Path(".")` in
  Python — the current directory — which `.exists()` as `True`. So the "is
  TLS configured" check in `server.py` (`if config.tls_cert and
  config.tls_key and config.tls_cert.exists() and ...`) evaluated **true**
  even with no TLS env vars set at all, and crashed with
  `ssl.load_cert_chain(".", ".")` → `IsADirectoryError` instead of taking
  the intended plaintext fallback. Fixed in `config.py` to return `None`
  when the env var is unset instead of `Path("")` — `None` is properly
  falsy and short-circuits before `.exists()` is ever called on it.

## Troubleshooting

- **`IsADirectoryError` on startup**: you're running a version of
  `config.py` predating the fix above, or something is passing a literal
  `.`/empty string as `XTEINK_RELAY_TLS_CERT`/`KEY`. Leave both unset for
  plaintext, or point them at a real cert/key pair.
- **`relay did not start` from `driver.sh`**: check
  `/tmp/run-relay-scratch/relay.log` — usually the port is already in use
  from a previous unstopped run; `driver.sh stop` first, or override
  `RUN_RELAY_PORT`.
