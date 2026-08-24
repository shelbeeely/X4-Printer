#!/usr/bin/env bash
# Installs the Xteink X4 print server on a Raspberry Pi (tested target:
# Raspberry Pi OS Bookworm on a Pi Zero W, but works on any Debian-family
# host). Idempotent: safe to re-run after a `git pull` to pick up code
# changes.
#
# What this does:
#   1. Installs system packages: cups, avahi-daemon, python3-venv, openssl.
#   2. Creates a dedicated `xteink-print` system user (member of `lp`, so it
#      can submit jobs to CUPS without being root).
#   3. Copies this checkout to /opt/xteink-print-server and creates a venv
#      there with requirements.txt installed.
#   4. Generates a self-signed TLS cert for the X4 sync API if one doesn't
#      already exist (re-run tools/gen_selfsigned_cert.py by hand if your
#      Pi's LAN IP changes).
#   5. Renders and installs the avahi mDNS service file so "Xteink X4" shows
#      up in print dialogs.
#   6. Installs and enables the systemd unit.
#
# What this does NOT do: configure your physical printer in CUPS. That's a
# one-time step specific to your printer model — see the "Configure your
# physical printer" section this script prints at the end, or CUPS's own
# web UI at http://<pi-host>:631/admin.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo ./install.sh)" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(dirname "$SCRIPT_DIR")"
INSTALL_DIR="/opt/xteink-print-server"
DATA_DIR="/var/lib/xteink-print-server"
SERVICE_USER="xteink-print"
PRINTER_NAME="${XTEINK_PRINTER_NAME:-Xteink X4}"
IPP_PORT="${XTEINK_IPP_PORT:-6310}"

echo "==> Installing system packages"
apt-get update -qq
apt-get install -y --no-install-recommends \
  cups cups-client avahi-daemon libnss-mdns \
  python3-venv python3-pip openssl

echo "==> Creating service user"
if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --home-dir "$INSTALL_DIR" --shell /usr/sbin/nologin --groups lp "$SERVICE_USER"
else
  usermod -aG lp "$SERVICE_USER"
fi

echo "==> Copying application to $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
rsync -a --delete \
  --exclude '.venv' --exclude '__pycache__' --exclude '.pytest_cache' --exclude 'tests' \
  "$SOURCE_DIR"/ "$INSTALL_DIR"/

echo "==> Creating virtualenv and installing Python dependencies"
if [[ ! -d "$INSTALL_DIR/.venv" ]]; then
  python3 -m venv "$INSTALL_DIR/.venv"
fi
"$INSTALL_DIR/.venv/bin/pip" install --upgrade pip -q
"$INSTALL_DIR/.venv/bin/pip" install -r "$INSTALL_DIR/requirements.txt" -q

echo "==> Creating data directories"
mkdir -p "$DATA_DIR"/{originals,xtc,tls}
chown -R "$SERVICE_USER":"$SERVICE_USER" "$DATA_DIR" "$INSTALL_DIR"

echo "==> Generating self-signed TLS certificate (sync API)"
if [[ ! -f "$DATA_DIR/tls/server.crt" ]]; then
  LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
  sudo -u "$SERVICE_USER" "$INSTALL_DIR/.venv/bin/python" "$INSTALL_DIR/tools/gen_selfsigned_cert.py" \
    --out-dir "$DATA_DIR/tls" --hostname "$(hostname)" ${LAN_IP:+--ip "$LAN_IP"}
else
  echo "    (existing certificate found, leaving in place)"
fi

echo "==> Installing mDNS/DNS-SD advertisement"
PRINTER_UUID="$(python3 -c 'import uuid; print(uuid.uuid4())')"
sed \
  -e "s/@@PRINTER_NAME@@/${PRINTER_NAME}/g" \
  -e "s/@@IPP_PORT@@/${IPP_PORT}/g" \
  -e "s/@@PRINTER_UUID@@/${PRINTER_UUID}/g" \
  "$SOURCE_DIR/install/avahi/xteink-x4-ipp.service.template" \
  > /etc/avahi/services/xteink-x4-ipp.service
systemctl restart avahi-daemon

echo "==> Installing systemd unit"
install -m 0644 "$SOURCE_DIR/install/xteink-print-server.service" /etc/systemd/system/xteink-print-server.service
systemctl daemon-reload
systemctl enable xteink-print-server.service
systemctl restart xteink-print-server.service

echo
echo "============================================================"
echo " Xteink X4 print server installed."
echo "============================================================"
echo
echo "1. Configure your physical printer in CUPS (one-time), e.g.:"
echo "     lpinfo -v                      # list detected printers"
echo "     lpadmin -p MyPrinter -E -v <device-uri-from-above> -m everywhere"
echo "   or use the CUPS web UI: http://$(hostname -I 2>/dev/null | awk '{print $1}'):631/admin"
echo
echo "2. Point the print server at that queue:"
echo "     sudo systemctl edit xteink-print-server.service"
echo "     # add: [Service]\\nEnvironment=XTEINK_CUPS_QUEUE=MyPrinter"
echo "     sudo systemctl restart xteink-print-server.service"
echo
echo "3. Pair each X4:"
echo "     sudo -u $SERVICE_USER $INSTALL_DIR/.venv/bin/python $INSTALL_DIR/tools/pair_device.py \\"
echo "       --name \"Kitchen X4\" --pi-host $(hostname -I 2>/dev/null | awk '{print $1}') --out ./device.json"
echo "   then copy device.json -> X4 SD card /system/device.json, and"
echo "   $DATA_DIR/tls/server.crt -> X4 SD card /system/pi_ca.pem (see docs/setup-x4.md)."
echo
echo "4. (Optional) Enable remote approval: see docs/relay.md, then set"
echo "   XTEINK_RELAY_URL/XTEINK_RELAY_ACCOUNT_ID/XTEINK_RELAY_ACCOUNT_TOKEN"
echo "   the same way as step 2."
echo
echo "\"$PRINTER_NAME\" should now appear in print dialogs on your LAN."
