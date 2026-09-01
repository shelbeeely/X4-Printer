#!/usr/bin/env bash
# Installs the Focusink relay on any small Debian-family host (a $5/mo VPS is
# plenty — see docs/relay.md). Idempotent: safe to re-run after `git pull`.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo ./install.sh)" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(dirname "$SCRIPT_DIR")"
INSTALL_DIR="/opt/focusink-relay"
DATA_DIR="/var/lib/focusink-relay"
SERVICE_USER="focusink-relay"

echo "==> Installing system packages"
apt-get update -qq
apt-get install -y --no-install-recommends python3-venv python3-pip

echo "==> Creating service user"
if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --home-dir "$INSTALL_DIR" --shell /usr/sbin/nologin "$SERVICE_USER"
fi

echo "==> Copying application to $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
rsync -a --delete \
  --exclude '.venv' --exclude '__pycache__' --exclude '.pytest_cache' --exclude 'tests' \
  "$SOURCE_DIR"/ "$INSTALL_DIR"/

echo "==> Creating virtualenv"
if [[ ! -d "$INSTALL_DIR/.venv" ]]; then
  python3 -m venv "$INSTALL_DIR/.venv"
fi
"$INSTALL_DIR/.venv/bin/pip" install --upgrade pip -q
# requirements.txt is intentionally empty (pure stdlib) but kept for parity
# with pi-server's install flow and in case future optional deps are added.
"$INSTALL_DIR/.venv/bin/pip" install -r "$INSTALL_DIR/requirements.txt" -q || true

echo "==> Creating data directory"
mkdir -p "$DATA_DIR"
chown -R "$SERVICE_USER":"$SERVICE_USER" "$DATA_DIR" "$INSTALL_DIR"

echo "==> Installing systemd unit"
install -m 0644 "$SOURCE_DIR/install/focusink-relay.service" /etc/systemd/system/focusink-relay.service
systemctl daemon-reload
systemctl enable focusink-relay.service
systemctl restart focusink-relay.service

echo
echo "============================================================"
echo " Focusink relay installed."
echo "============================================================"
echo
echo "TLS: this unit runs the relay in plaintext by default. Either:"
echo "  (a) point FOCUSINK_RELAY_TLS_CERT/FOCUSINK_RELAY_TLS_KEY at a real"
echo "      certificate (e.g. certbot) and restart the service, or"
echo "  (b) put nginx/caddy in front on 443 and reverse-proxy to"
echo "      127.0.0.1:8843 — recommended, see docs/relay.md."
echo "Do not expose FOCUSINK_RELAY_PORT to the internet without one of the two."
echo
echo "Create an account for your household:"
echo "  sudo -u $SERVICE_USER $INSTALL_DIR/.venv/bin/python $INSTALL_DIR/tools/create_account.py --name \"My Household\""
echo
echo "Then configure pi-server (FOCUSINK_RELAY_URL/ACCOUNT_ID/ACCOUNT_TOKEN)"
echo "and re-pair devices so they pick up the relay fields — see docs/relay.md."
