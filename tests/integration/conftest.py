import stat
import sys
import threading
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "pi-server"))
sys.path.insert(0, str(REPO_ROOT / "relay"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from relay_server.app import RelayServer  # noqa: E402
from relay_server.config import RelayConfig  # noqa: E402
from relay_server.db import RelayDatabase  # noqa: E402
from relay_server.util import hash_token as relay_hash_token  # noqa: E402
from relay_server.util import new_token  # noqa: E402
from xteink_print_server.config import Config  # noqa: E402
from xteink_print_server.db import Database  # noqa: E402
from xteink_print_server.ipp_server import IppServer  # noqa: E402
from xteink_print_server.sync_api import SyncApiServer  # noqa: E402
from xteink_print_server.util import hash_token as pi_hash_token  # noqa: E402
from xteink_print_server.util import new_token as pi_new_token  # noqa: E402


class FakeLpBinary(str):
    log_path: Path


@pytest.fixture
def fake_lp_binary(tmp_path: Path) -> FakeLpBinary:
    script = tmp_path / "fake_lp"
    log = tmp_path / "fake_lp_calls.log"
    script.write_text(f"#!/bin/sh\necho \"$@\" >> {log}\necho 'request id is xteink-print-inbox-1 (1 file(s))'\n")
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    wrapped = FakeLpBinary(str(script))
    wrapped.log_path = log
    return wrapped


@pytest.fixture
def pi_stack(tmp_path: Path, fake_lp_binary: FakeLpBinary):
    """Boots a real IPP server + sync API (plain HTTP, no TLS — see
    module docstring in test_end_to_end.py for why that's fine for this
    test) against a fresh database, and a paired device."""
    config = Config(
        data_dir=tmp_path / "pi-data",
        cups_queue="TestPrinter",
        lp_binary=str(fake_lp_binary),
        ipp_host="127.0.0.1",
        ipp_port=0,
        sync_host="127.0.0.1",
        sync_port=0,
        panel_width=800,
        panel_height=480,
        tls_cert=tmp_path / "pi-data" / "tls" / "server.crt",
        tls_key=tmp_path / "pi-data" / "tls" / "server.key",
    )
    config.ensure_dirs()
    db = Database(config.db_path)

    ipp_server = IppServer(config, db)
    sync_server = SyncApiServer(config, db)
    threading.Thread(target=ipp_server.serve_forever, daemon=True).start()
    threading.Thread(target=sync_server.serve_forever, daemon=True).start()
    time.sleep(0.05)

    ipp_port = ipp_server.server_address[1]
    sync_port = sync_server.server_address[1]

    device_id = "dev-integration"
    device_token = pi_new_token()
    db.register_device(device_id, pi_hash_token(device_token), "Integration Test Device", None)

    yield {
        "config": config,
        "db": db,
        "ipp_url": f"http://127.0.0.1:{ipp_port}/ipp/print",
        "sync_base_url": f"http://127.0.0.1:{sync_port}/api/v1",
        "device_id": device_id,
        "device_token": device_token,
        "fake_lp_binary": fake_lp_binary,
    }

    ipp_server.shutdown()
    sync_server.shutdown()


@pytest.fixture
def relay_stack(tmp_path: Path):
    config = RelayConfig(data_dir=tmp_path / "relay-data", host="127.0.0.1", port=0)
    config.ensure_dirs()
    db = RelayDatabase(config.db_path)
    server = RelayServer(config, db)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    time.sleep(0.05)
    port = server.server_address[1]

    account_id = "acct-integration"
    account_token = new_token()
    db.create_account(account_id, relay_hash_token(account_token), "Integration Household")

    yield {
        "base_url": f"http://127.0.0.1:{port}/relay/v1",
        "account_id": account_id,
        "account_token": account_token,
        "db": db,
    }
    server.shutdown()
