#!/usr/bin/env python3
"""Creates a relay account (one per household/Pi) and prints the
account_id + token to hand to pi-server's FOCUSINK_RELAY_ACCOUNT_ID /
FOCUSINK_RELAY_ACCOUNT_TOKEN config and to pair_device.py for each X4 in that
household. The relay is self-hosted by the same person who owns the Pi in
this prototype, so account creation is an operator-run CLI, not a public
signup flow — see docs/relay.md for scaling this to a shared/multi-tenant
deployment.
"""

from __future__ import annotations

import argparse
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from relay_server.config import load_config  # noqa: E402
from relay_server.db import RelayDatabase  # noqa: E402
from relay_server.util import hash_token, new_token  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="Human-readable account name, e.g. 'Smith Household'")
    args = parser.parse_args()

    config = load_config()
    db = RelayDatabase(config.db_path)

    account_id = f"acct-{uuid.uuid4().hex[:12]}"
    token = new_token()
    db.create_account(account_id, hash_token(token), args.name)

    print(f"account_id:    {account_id}")
    print(f"account_token: {token}")
    print()
    print("Add to pi-server's environment (see pi-server/install/focusink-server.service):")
    print(f"  FOCUSINK_RELAY_ACCOUNT_ID={account_id}")
    print(f"  FOCUSINK_RELAY_ACCOUNT_TOKEN={token}")
    print(f"  FOCUSINK_RELAY_URL=https://<this-relay-host>:{config.port}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
