# Setting Up the Raspberry Pi Print Server

Target: Raspberry Pi Zero W (or any Pi/Debian host) running Raspberry Pi OS
(Bookworm or newer), on the same LAN as the X4.

## Prerequisites

- A physical network/USB printer already reachable from the Pi (or plugged
  into it) — this project forwards *to* CUPS, it doesn't replace your
  printer driver.
- The Pi has internet access during install (to `apt-get install` packages
  and, for the manual install, `pip install` PyMuPDF/Pillow — the Docker
  path builds these into the image instead).

## Docker install (recommended)

> **A note for a genuine Raspberry Pi Zero W**: 512MB RAM is already
> shared with CUPS, avahi, and the OS itself, and Docker's own overhead
> (dockerd/containerd, image layers) is a real cost on top of that on a
> single-core device this constrained. If RAM is tight on your specific
> hardware, skip to "Manual install (no Docker)" below, which avoids that
> overhead entirely. On a Pi 3/4/5 (1GB+ RAM) this is a non-issue.

CUPS and avahi-daemon still run directly on the Pi's host OS either way —
they need real USB/driver access and the host's own network stack for
mDNS, which containerizing wouldn't meaningfully simplify (see
`pi-server/install/docker-host-setup.sh`'s header comment). Docker only
replaces the Python app itself (the venv + systemd unit the manual install
sets up).

### 1. Host prerequisites

```sh
git clone <this-repo>
cd X4-Printer
sudo ./pi-server/install/docker-host-setup.sh
```

Installs CUPS + avahi-daemon and renders the mDNS advertisement file. Full
detail: `pi-server/install/docker-host-setup.sh` (short — read it before
running it, as with any installer that needs sudo). Docker itself isn't
installed by this script — see
[docs.docker.com/engine/install/debian](https://docs.docker.com/engine/install/debian/)
(or `raspbian`, same instructions) if you don't already have it.

### 2. Configure your physical printer in CUPS

```sh
lpinfo -v                                          # list detected printers
sudo lpadmin -p MyPrinter -E -v <device-uri> -m everywhere
```

or use the CUPS web UI at `http://<pi-host>:631/admin`. Most printers made
in the last ~10 years support IPP-Everywhere/AirPrint and the `-m
everywhere` driverless setup above just works; older printers need a real
PPD (see `lpinfo -m` and your printer's model page).

### 3. Configure and start the print server

```sh
cp .env.example .env
$EDITOR .env    # at minimum, set XTEINK_CUPS_QUEUE=MyPrinter from step 2
docker compose up -d --build
```

Everything durable (job queue database, originals/converted files, the TLS
cert) lands in `./data` — one directory to back up. Edit `.env` and
`docker compose up -d --build` again any time to pick up a change (no
separate restart needed, `up` handles it).

### 4. Confirm the virtual printer is visible

From any machine on the same LAN: open the normal print dialog (macOS
"Add Printer", Windows "Add a printer or scanner", Linux
`system-config-printer`/GNOME Settings → Printers, or just print from any
app). **"Xteink X4"** should appear automatically — no driver install, no
IP address to type in. Print a test PDF; it should complete instantly (IPP
accepts it immediately — the actual physical print only happens once
approved from the device or, before any device exists yet, never — see
step 6).

If it doesn't show up: `avahi-browse -a` should list `_ipp._tcp` for
"Xteink X4 @ <hostname>"; if it doesn't, check `systemctl status
avahi-daemon` on the host and that the client machine is on the same L2
segment (mDNS doesn't cross VLANs/subnets without a reflector).

### 5. Pair your first X4

```sh
docker compose exec pi-server python tools/pair_device.py \
  --name "Kitchen X4" --pi-host <pi-lan-ip-or-hostname> --out /data/kitchen-x4.json
```

This prints a `device_id` and registers it in the server's database. Copy
the resulting files off the Pi to the X4's SD card (see
`docs/setup-x4.md`):

- `./data/kitchen-x4.json` → `/system/device.json`
- `./data/tls/server.crt` → `/system/pi_ca.pem`

### 6. (Optional) Remote approval away from home

See `docs/relay.md`.

### 7. (Optional) Admin web console

A dashboard for jobs/devices/approvals/calendars/Wi-Fi networks, plus
live-editable CUPS queue, retention, and relay settings — disabled unless
`XTEINK_ADMIN_PASSWORD` is set in `.env` (step 3). Once set, open
`http://<pi-host>:8090/` (or `https://` once the TLS cert exists, which it
will after step 3's first start — see `docker/pi-server-entrypoint.sh`)
and log in with any username and that password — the browser's own login
prompt handles it. See `docs/security.md` "Admin web console" for exactly
what it can do and its trust model before exposing it beyond your own
LAN.

The **Calendars & Wi-Fi** tab is the primary way to manage both lists once
a device is paired — entries there sync to every paired X4 on its next
wake, no SD card round-trip needed (see `docs/protocol.md` §1.6). A brand
new, unpaired device still needs *some* Wi-Fi network hand-written to its
SD card for its very first sync (see `docs/setup-x4.md`) — this tab is
for managing the list from then on, including pushing down additional
networks.

### 8. Verify end-to-end without hardware

`tools/simulate_x4.py` (repo root, run from your own machine — not inside
the container) acts as a fake X4 over the same sync protocol the firmware
uses — handy for confirming the whole pipeline (print → convert → sync →
approve → CUPS forward) works before you have a physical device paired, or
for CI:

```sh
python3 tools/simulate_x4.py --pi-base-url https://<pi-host>:8443/api/v1 \
  --device-id <from pair_device.py> --device-token <from pair_device.py> \
  --ca-cert ./data/tls/server.crt \
  list-jobs
```

See `tools/simulate_x4.py --help` for the full command set
(`list-jobs`, `download`, `approve`), and `tests/integration/test_end_to_end.py`
for the automated version of the same flow.

## Manual install (no Docker)

Installs everything (CUPS, avahi, the Python app) directly on the host as
a systemd service — no Docker involved. Prefer this on a genuine Pi Zero W
if the Docker path's overhead (see above) matters for your setup, or if
you'd simply rather not run Docker at all.

### 1. Install

```sh
git clone <this-repo>
cd X4-Printer/pi-server
sudo ./install/install.sh
```

This installs CUPS, avahi-daemon, creates a `xteink-print` system user,
sets up a Python virtualenv at `/opt/xteink-print-server/.venv`, generates a
self-signed TLS certificate for the sync API, installs the mDNS
advertisement, and starts the `xteink-print-server` systemd service. Full
detail: `pi-server/install/install.sh` (it's short — read it before running
it, as with any installer that needs sudo).

### 2. Configure your physical printer in CUPS

If you haven't already:

```sh
lpinfo -v                                          # list detected printers
sudo lpadmin -p MyPrinter -E -v <device-uri> -m everywhere
```

or use the CUPS web UI at `http://<pi-host>:631/admin`. Most printers made
in the last ~10 years support IPP-Everywhere/AirPrint and the `-m
everywhere` driverless setup above just works; older printers need a real
PPD (see `lpinfo -m` and your printer's model page).

Then tell the print server which queue to use:

```sh
sudo systemctl edit xteink-print-server.service
```

Add:

```ini
[Service]
Environment=XTEINK_CUPS_QUEUE=MyPrinter
```

```sh
sudo systemctl restart xteink-print-server.service
```

### 3. Confirm the virtual printer is visible

From any machine on the same LAN: open the normal print dialog (macOS
"Add Printer", Windows "Add a printer or scanner", Linux
`system-config-printer`/GNOME Settings → Printers, or just print from any
app). **"Xteink X4"** should appear automatically — no driver install, no
IP address to type in. Print a test PDF; it should complete instantly (IPP
accepts it immediately — the actual physical print only happens once
approved from the device or, before any device exists yet, never — see
step 5).

If it doesn't show up: `avahi-browse -a` should list `_ipp._tcp` for
"Xteink X4 @ <hostname>"; if it doesn't, check `systemctl status
avahi-daemon` and that the client machine is on the same L2 segment (mDNS
doesn't cross VLANs/subnets without a reflector).

### 4. Pair your first X4

```sh
sudo -u xteink-print /opt/xteink-print-server/.venv/bin/python \
  /opt/xteink-print-server/tools/pair_device.py \
  --name "Kitchen X4" --pi-host <pi-lan-ip-or-hostname> --out ./kitchen-x4.json
```

This prints a `device_id` and registers it in the server's database. Copy
the resulting files to the X4's SD card (see `docs/setup-x4.md`):

- `kitchen-x4.json` → `/system/device.json`
- `/var/lib/xteink-print-server/tls/server.crt` → `/system/pi_ca.pem`

### 5. (Optional) Remote approval away from home

See `docs/relay.md`.

### 6. (Optional) Admin web console

A dashboard for jobs/devices/approvals/calendars/Wi-Fi networks, plus
live-editable CUPS queue, retention, and relay settings — disabled unless
you set a password:

```sh
sudo systemctl edit xteink-print-server.service
```

Add:

```ini
[Service]
Environment=XTEINK_ADMIN_PASSWORD=<a strong password>
```

```sh
sudo systemctl restart xteink-print-server.service
```

Then open `http://<pi-host>:8090/` (or `https://` once the TLS cert from
step 1 exists, which it will after a normal install) and log in with any
username and that password — the browser's own login prompt handles it.
See `docs/security.md` "Admin web console" for exactly what it can do and
its trust model before exposing it beyond your own LAN. The **Calendars &
Wi-Fi** tab there is the primary way to manage both lists once a device is
paired — see the Docker section's step 7 above for what it does.

### 7. Verify end-to-end without hardware

`tools/simulate_x4.py` (repo root) acts as a fake X4 over the same sync
protocol the firmware uses — handy for confirming the whole pipeline
(print → convert → sync → approve → CUPS forward) works before you have a
physical device paired, or for CI:

```sh
python3 tools/simulate_x4.py --pi-base-url https://<pi-host>:8443/api/v1 \
  --device-id <from pair_device.py> --device-token <from pair_device.py> \
  --ca-cert /var/lib/xteink-print-server/tls/server.crt \
  list-jobs
```

See `tools/simulate_x4.py --help` for the full command set
(`list-jobs`, `download`, `approve`), and `tests/integration/test_end_to_end.py`
for the automated version of the same flow.
