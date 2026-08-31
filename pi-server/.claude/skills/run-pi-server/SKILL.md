---
name: run-pi-server
description: Build, run, and drive the X4 Print Inbox Pi server (IPP printer listener, X4 sync API, admin console web UI). Use when asked to start pi-server, run/print an IPP job, exercise the sync API, screenshot the admin console, or smoke-test the Pi server end to end.
---

pi-server is a real multi-threaded Python HTTP service (`python -m
xteink_print_server.server`) with no dev-server/prod split. Drive it via
`driver.sh` in this skill directory: it launches a real instance, submits a
real IPP print job, pairs a fake X4 device and syncs+approves through the
real sync API (`tools/simulate_x4.py`), and screenshots the admin console
with an authenticated Playwright session.

All paths below are relative to `pi-server/`.

## Prerequisites

Nothing beyond Python 3 + a venv — this project deliberately has no
external framework dependency (stdlib `http.server` for both listeners).

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # PyMuPDF, Pillow
```

`driver.sh start`/`smoke` create this venv automatically on first run if
it doesn't exist yet.

For the admin-console screenshot step, Playwright's Python package plus
the container's pre-installed Chromium:

```bash
.venv/bin/pip install playwright   # do NOT run `playwright install` --
                                    # use the pre-installed browser below
```

## Build

None — pure Python, no compile/bundle step.

## Run (agent path)

```bash
.claude/skills/run-pi-server/driver.sh smoke
```

This is the one command to run. It: generates a throwaway self-signed TLS
cert, launches the server in the background on non-default ports, waits
for its "ready" log line, submits a real IPP Print-Job (both a normal and
a landscape-strip XTC rendering get generated), pairs a fake X4 device,
downloads+verifies+acks both XTC variants via the real sync API, submits a
"keep" approval, curls the admin API, and screenshots the admin console's
Jobs tab to `/tmp/run-pi-server-scratch/admin_console.png`. It leaves the
server running afterward — call `stop` when done.

For just launching a long-lived instance to poke at manually:

```bash
.claude/skills/run-pi-server/driver.sh start
# prints IPP/Sync/Admin URLs, the admin password, and the data/log dir paths
```

Stop it:

```bash
.claude/skills/run-pi-server/driver.sh stop
```

| command | what it does |
|---|---|
| `driver.sh start` | Launch in the background (idempotent — no-ops if already running), print connection info |
| `driver.sh smoke` | `start` + full drive-through (print, sync, approve, admin screenshot) + report |
| `driver.sh stop` | Kill the background instance |

Once a `start`/`smoke` instance is up, drive individual pieces directly:

```bash
# A real IPP print job:
.venv/bin/python .claude/skills/run-pi-server/submit_print_job.py \
  http://127.0.0.1:16310/ipp/print "My Job" 2   # url, title, page count

# Pair a fake X4 + drive the sync API (list/download/verify/ack/approve):
.venv/bin/python tools/pair_device.py --name "Test X4" --pi-host 127.0.0.1 \
  --out /tmp/device.json   # needs XTEINK_SYNC_PORT etc. set to match — see driver.sh
.venv/bin/python ../tools/simulate_x4.py --pairing-file /tmp/device.json \
  --ca-cert /tmp/run-pi-server-scratch/pi-data/tls/server.crt sync --download-dir /tmp/inbox

# Screenshot the admin console:
.venv/bin/python .claude/skills/run-pi-server/screenshot_admin.py \
  https://127.0.0.1:18090 smoketest123 /tmp/admin.png
```

Override ports/password via env vars before calling `driver.sh` (see the
top of the script): `RUN_PI_SERVER_IPP_PORT`, `RUN_PI_SERVER_SYNC_PORT`,
`RUN_PI_SERVER_ADMIN_PORT`, `RUN_PI_SERVER_ADMIN_PASSWORD`,
`RUN_PI_SERVER_SCRATCH` (default `/tmp/run-pi-server-scratch`).

## Run (human path)

```bash
XTEINK_ADMIN_PASSWORD=changeme python3 -m xteink_print_server.server
```

Blocks in the foreground (real deployments run it under the systemd unit
in `install/`). `Ctrl-C` to stop. TLS defaults to a **fixed absolute path**
(`/var/lib/xteink-print-server/tls/...`), not derived from
`XTEINK_DATA_DIR` — set `XTEINK_TLS_CERT`/`XTEINK_TLS_KEY` explicitly for a
non-production data dir, or generate a cert with `tools/gen_selfsigned_cert.py`.

## Test

```bash
.venv/bin/python -m pytest -q
```

73 tests, all pass. Also `cd .. && .venv/bin/python -m pytest tests/integration -q`
(2 end-to-end tests using real `IppServer`/`SyncApiServer`/`RelayServer`
instances) from the repo root with this venv active — 2 pass.

## Gotchas

- **TLS cert defaults to a fixed system path, not your data dir.**
  `Config.tls_cert`/`tls_key` default to `/var/lib/xteink-print-server/tls/...`
  regardless of `XTEINK_DATA_DIR` — always set `XTEINK_TLS_CERT`/`XTEINK_TLS_KEY`
  explicitly for a scratch/test run (`driver.sh` does this for you).
- **`gen_selfsigned_cert.py`'s default cert is only valid for the
  container's hostname**, not `127.0.0.1`. Pass `--ip 127.0.0.1` explicitly
  for any loopback test, or `simulate_x4.py`/`urllib` will fail with
  `CERTIFICATE_VERIFY_FAILED: IP address mismatch`.
- **`pair_device.py` always writes `https://` into `pi_base_url`**
  regardless of whether TLS is actually configured on the target server —
  if you skip TLS setup, pairing-file-based sync tooling will fail to
  connect; there's no plaintext-sync pairing path.
- **Backgrounding with `nohup cmd & disown` in one shell call doesn't
  reliably survive** in some agent harnesses (the process can vanish
  between tool calls with no error). `(cmd &)` in its own subshell, as
  `driver.sh` does, is reliable.
- **Admin console needs `XTEINK_ADMIN_PASSWORD` set** or the whole admin
  API thread never starts (`server.py` logs "admin console disabled" and
  moves on) — not an error, just silently absent.

## Troubleshooting

- **`IsADirectoryError` mentioning `tls`**: you didn't set
  `XTEINK_TLS_CERT`/`XTEINK_TLS_KEY` and the default path's parent doesn't
  exist as expected on this machine, or a stray `.` got passed as a path —
  see the TLS gotcha above.
- **`server did not become ready` from `driver.sh`**: check
  `/tmp/run-pi-server-scratch/server.log` — usually a port already in use
  from a previous unstoppped run; `driver.sh stop` first, or override the
  port env vars.
