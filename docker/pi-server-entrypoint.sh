#!/bin/sh
# Auto-generates a self-signed TLS cert for the sync/admin APIs on first
# start (pi-server/tools/gen_selfsigned_cert.py), the containerized
# equivalent of the explicit step install/install.sh runs before starting
# the bare-metal systemd service -- see docs/setup-pi.md's Docker section.
#
# Gated on XTEINK_AUTO_TLS=1 (set only by the production docker-compose.yml
# at the repo root) and deliberately unset in docker-compose.test.yml,
# which has no use for TLS and shouldn't gain a new startup side effect
# just from sharing this image. Skips entirely if a cert already exists at
# $XTEINK_TLS_CERT, so this only ever runs once per persistent /data volume
# -- re-run pi-server/tools/gen_selfsigned_cert.py by hand (see its own
# header comment) if the Pi's LAN IP changes later.
set -eu

if [ "${XTEINK_AUTO_TLS:-}" = "1" ]; then
  cert="${XTEINK_TLS_CERT:-/data/tls/server.crt}"
  key="${XTEINK_TLS_KEY:-/data/tls/server.key}"
  if [ ! -f "$cert" ] || [ ! -f "$key" ]; then
    out_dir="$(dirname "$cert")"
    mkdir -p "$out_dir"
    # network_mode: host means this container shares the Pi's real network
    # interfaces, so `hostname -I` returns the Pi's actual LAN IP(s), not a
    # container-internal address.
    lan_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
    if [ -n "$lan_ip" ]; then
      python tools/gen_selfsigned_cert.py --out-dir "$out_dir" --hostname "$(hostname)" --ip "$lan_ip"
    else
      python tools/gen_selfsigned_cert.py --out-dir "$out_dir" --hostname "$(hostname)"
    fi
  fi
fi

exec "$@"
