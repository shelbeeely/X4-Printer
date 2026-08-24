#!/usr/bin/env python3
"""Generates a self-signed TLS certificate for the X4 sync API.

The X4 firmware pins this certificate (copies server.crt to its SD card as
/system/pi_ca.pem during pairing, see pair_device.py and
docs/setup-x4.md) instead of trusting a public CA, since this endpoint is
only ever reached over the home LAN. Re-run this any time the Pi's
LAN IP/hostname changes materially, then re-pair devices (or just re-copy
the new server.crt to each device's SD card — the token doesn't change).
"""

from __future__ import annotations

import argparse
import ipaddress
import socket
import subprocess
import sys
from pathlib import Path


def _default_hostname() -> str:
    try:
        return socket.gethostname()
    except OSError:
        return "xteink-print-server"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, default=Path("/var/lib/xteink-print-server/tls"))
    parser.add_argument("--hostname", default=_default_hostname())
    parser.add_argument("--ip", action="append", default=[], help="Additional IP SAN (repeatable)")
    parser.add_argument("--days", type=int, default=3650)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    key_path = args.out_dir / "server.key"
    cert_path = args.out_dir / "server.crt"

    sans = [f"DNS:{args.hostname}", f"DNS:{args.hostname}.local"]
    for ip in args.ip:
        try:
            ipaddress.ip_address(ip)
        except ValueError:
            print(f"skipping invalid --ip {ip!r}", file=sys.stderr)
            continue
        sans.append(f"IP:{ip}")
    san_arg = ",".join(sans)

    cmd = [
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        str(key_path),
        "-out",
        str(cert_path),
        "-days",
        str(args.days),
        "-subj",
        f"/CN={args.hostname}",
        "-addext",
        f"subjectAltName={san_arg}",
    ]
    subprocess.run(cmd, check=True)
    key_path.chmod(0o600)
    print(f"wrote {cert_path} and {key_path}")
    print("Copy this certificate to each X4's SD card as /system/pi_ca.pem:")
    print(f"  cp {cert_path} /path/to/x4/sdcard/system/pi_ca.pem")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
