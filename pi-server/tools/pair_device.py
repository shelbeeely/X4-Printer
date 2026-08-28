#!/usr/bin/env python3
"""Pairs a new X4 device with this Pi: generates a device_id + bearer token,
registers it in jobs.db, and writes a pairing.json the user copies onto the
device's SD card at /system/device.json (see docs/setup-x4.md).

Usage:
    python3 tools/pair_device.py --name "Kitchen X4" --pi-host 192.168.1.42

If XTEINK_RELAY_URL/XTEINK_RELAY_ACCOUNT_ID/XTEINK_RELAY_ACCOUNT_TOKEN are
set in the environment (same account used by relay_client.py), the relay
fields are included in the pairing file too, so the device can reach the
relay directly when away from home. Omit them to pair a device for
LAN-only sync.
"""

from __future__ import annotations

import argparse
import json
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from xteink_print_server.config import load_config  # noqa: E402
from xteink_print_server.db import Database  # noqa: E402
from xteink_print_server.util import hash_token, new_token  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True, help="Human-readable device name, e.g. 'Kitchen X4'")
    parser.add_argument("--pi-host", required=True, help="Hostname or IP the X4 should reach this Pi at on the LAN")
    parser.add_argument("--out", type=Path, default=Path("./device.json"), help="Where to write the pairing file")
    args = parser.parse_args()

    config = load_config()
    db = Database(config.db_path)

    device_id = f"dev-{uuid.uuid4().hex[:12]}"
    token = new_token()
    db.register_device(device_id, hash_token(token), args.name, config.relay_account_id or None)

    pairing = {
        "device_id": device_id,
        "device_token": token,
        "device_name": args.name,
        "pi_base_url": f"https://{args.pi_host}:{config.sync_port}/api/v1",
    }
    if config.relay_url and config.relay_account_id and config.relay_account_token:
        pairing["relay_base_url"] = f"{config.relay_url.rstrip('/')}/relay/v1"
        pairing["relay_account_id"] = config.relay_account_id
        pairing["relay_account_token"] = config.relay_account_token
    if config.admin_password:
        # Lets the X4's on-device web UI (station mode only) link a phone
        # straight to the Pi's original-document route — see
        # docs/architecture.md "On-device Web UI full-document preview".
        # Omitted entirely when the admin console itself is disabled
        # (empty admin_password), same conditional-inclusion pattern as
        # the relay fields above.
        admin_scheme = "https" if config.tls_cert.exists() and config.tls_key.exists() else "http"
        pairing["pi_admin_base_url"] = f"{admin_scheme}://{args.pi_host}:{config.admin_port}"

    args.out.write_text(json.dumps(pairing, indent=2) + "\n")
    print(f"Paired device_id={device_id}")
    print(f"Wrote {args.out}")
    print("Next steps:")
    print(f"  1. Copy {args.out} to the X4's SD card as /system/device.json")
    print(f"  2. Copy {config.tls_cert} to the X4's SD card as /system/pi_ca.pem")
    print("  3. Set up Wi-Fi credentials at /system/wifi.json (see docs/setup-x4.md), or use the on-device Wi-Fi setup screen")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
