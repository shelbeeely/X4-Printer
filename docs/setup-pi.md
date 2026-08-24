# Setting Up the Raspberry Pi Print Server

Target: Raspberry Pi Zero W (or any Pi/Debian host) running Raspberry Pi OS
(Bookworm or newer), on the same LAN as the X4.

## 1. Prerequisites

- A physical network/USB printer already reachable from the Pi (or plugged
  into it) — this project forwards *to* CUPS, it doesn't replace your
  printer driver.
- The Pi has internet access during install (to `apt-get install` packages
  and `pip install` PyMuPDF/Pillow).

## 2. Install

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

## 3. Configure your physical printer in CUPS

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

## 4. Confirm the virtual printer is visible

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
avahi-daemon` and that the client machine is on the same L2 segment (mDNS
doesn't cross VLANs/subnets without a reflector).

## 5. Pair your first X4

```sh
sudo -u xteink-print /opt/xteink-print-server/.venv/bin/python \
  /opt/xteink-print-server/tools/pair_device.py \
  --name "Kitchen X4" --pi-host <pi-lan-ip-or-hostname> --out ./kitchen-x4.json
```

This prints a `device_id` and registers it in the server's database. Copy
the resulting files to the X4's SD card (see `docs/setup-x4.md`):

- `kitchen-x4.json` → `/system/device.json`
- `/var/lib/xteink-print-server/tls/server.crt` → `/system/pi_ca.pem`

## 6. (Optional) Remote approval away from home

See `docs/relay.md`.

## 7. Verify end-to-end without hardware

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
