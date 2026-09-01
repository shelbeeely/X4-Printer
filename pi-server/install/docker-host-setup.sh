#!/usr/bin/env bash
# One-time HOST prerequisites for the Docker deployment (docker-compose.yml
# at the repo root; see docs/setup-pi.md). Run this once, then
# `docker compose up -d --build` for everything else.
#
# Why this exists at all instead of putting everything in the container:
# CUPS needs real USB/network access to your physical printer's driver,
# and avahi-daemon needs the host's actual network stack/D-Bus for mDNS
# advertisement (the "Focusink" entry in print dialogs) -- containerizing
# either would trade a real simplification for a lot of fragility (device
# passthrough, D-Bus socket mounts) for something this small. So CUPS and
# avahi stay host packages under BOTH the Docker and manual-install paths;
# this script is the Docker path's equivalent of install/install.sh's
# steps 1 and 5 (system packages + mDNS service file) with the venv/
# systemd steps (3, 6) left out, since Docker replaces those.
#
# Idempotent: safe to re-run.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo ./docker-host-setup.sh)" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(dirname "$SCRIPT_DIR")"
PRINTER_NAME="${FOCUSINK_PRINTER_NAME:-Focusink}"
IPP_PORT="${FOCUSINK_IPP_PORT:-6310}"

echo "==> Installing system packages (CUPS + avahi -- see this script's header comment for why these stay host packages)"
apt-get update -qq
apt-get install -y --no-install-recommends \
  cups cups-client avahi-daemon libnss-mdns

echo "==> Installing mDNS/DNS-SD advertisement"
PRINTER_UUID="$(python3 -c 'import uuid; print(uuid.uuid4())' 2>/dev/null || cat /proc/sys/kernel/random/uuid)"
sed \
  -e "s/@@PRINTER_NAME@@/${PRINTER_NAME}/g" \
  -e "s/@@IPP_PORT@@/${IPP_PORT}/g" \
  -e "s/@@PRINTER_UUID@@/${PRINTER_UUID}/g" \
  "$SOURCE_DIR/install/avahi/focusink-x4-ipp.service.template" \
  > /etc/avahi/services/focusink-x4-ipp.service
systemctl restart avahi-daemon

echo
echo "============================================================"
echo " Host prerequisites installed."
echo "============================================================"
echo
echo "1. Configure your physical printer in CUPS (one-time), e.g.:"
echo "     lpinfo -v                      # list detected printers"
echo "     lpadmin -p MyPrinter -E -v <device-uri-from-above> -m everywhere"
echo "   or use the CUPS web UI: http://$(hostname -I 2>/dev/null | awk '{print $1}'):631/admin"
echo
echo "2. cd $SOURCE_DIR/.. && cp .env.example .env, fill in FOCUSINK_CUPS_QUEUE"
echo "   with the queue name from step 1 (and FOCUSINK_ADMIN_PASSWORD /"
echo "   relay settings if you want those -- see docs/setup-pi.md)."
echo
echo "3. docker compose up -d --build"
echo
echo "\"$PRINTER_NAME\" should then appear in print dialogs on your LAN."
