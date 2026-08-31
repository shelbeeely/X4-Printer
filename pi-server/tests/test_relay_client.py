"""Unit tests for relay_client.py -- the Pi-side poller for the optional
cloud relay (docs/protocol.md §2). Unlike test_end_to_end.py (which drives
this through a real RelayServer instance as part of the full pipeline),
these tests fake the relay's HTTP surface directly so relay_client's own
polling/lifecycle/error-handling logic is exercised in isolation.
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

from xteink_print_server.config import Config
from xteink_print_server.db import Database
from xteink_print_server.relay_client import RelayClient

ACCOUNT_ID = "acct-1"
DEVICE_ID = "dev-1"


def _insert_job(db: Database, config: Config, title="Doc") -> str:
    original = config.originals_dir / "orig.pdf"
    original.write_bytes(b"%PDF-fake")
    return db.insert_job(
        title=title,
        source="ipp",
        original_path=str(original),
        original_mime="application/pdf",
        original_bytes=original.stat().st_size,
        xtc_path=str(config.xtc_dir / "out.xtc"),
        xtc_bytes=100,
        xtc_sha256="deadbeef",
        page_count=1,
    )


class _FakeRelayHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # noqa: ANN002 - silence default request logging
        pass

    def _write_json(self, status: int, body: dict) -> None:
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):  # noqa: N802 - http.server's naming convention
        self.server.requests.append(("GET", self.path))  # type: ignore[attr-defined]
        if self.path.endswith("/approvals/pending"):
            self._write_json(200, {"approvals": self.server.pending_approvals})  # type: ignore[attr-defined]
            return
        self._write_json(404, {"error": "not found"})

    def do_POST(self):  # noqa: N802
        self.server.requests.append(("POST", self.path))  # type: ignore[attr-defined]
        if self.path.endswith("/ack"):
            if self.server.ack_should_fail:  # type: ignore[attr-defined]
                self._write_json(500, {"error": "ack failed"})
                return
            self._write_json(200, {"status": "ok"})
            return
        self._write_json(404, {"error": "not found"})


class _FakeRelayServer(HTTPServer):
    requests: list
    pending_approvals: list
    ack_should_fail: bool


@pytest.fixture
def fake_relay():
    server = _FakeRelayServer(("127.0.0.1", 0), _FakeRelayHandler)
    server.requests = []
    server.pending_approvals = []
    server.ack_should_fail = False
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    thread.join(timeout=2)


@pytest.fixture
def relay_config(config: Config, fake_relay) -> Config:
    port = fake_relay.server_address[1]
    config.relay_url = f"http://127.0.0.1:{port}"
    config.relay_account_id = ACCOUNT_ID
    config.relay_account_token = "relay-token"
    config.relay_poll_interval_seconds = 60
    return config


def test_enabled_requires_full_relay_config(config: Config, db: Database):
    client = RelayClient(config, db)
    assert client.enabled is False

    config.relay_url = "http://example.invalid"
    assert client.enabled is False  # account id/token still missing

    config.relay_account_id = ACCOUNT_ID
    config.relay_account_token = "tok"
    assert client.enabled is True


def test_poll_once_applies_and_acks(relay_config: Config, db: Database, fake_relay, fake_lp_binary):
    job_id = _insert_job(db, relay_config)
    fake_relay.pending_approvals = [
        {
            "approval_id": "appr-relay-1",
            "device_id": DEVICE_ID,
            "job_id": job_id,
            "action": "print",
            "created_at": 1737590000,
        }
    ]

    client = RelayClient(relay_config, db)
    applied = client.poll_once()

    assert applied == 1
    job = db.get_job(job_id)
    assert job["status"] == "kept"

    call_log = fake_lp_binary.log_path.read_text().strip().splitlines()  # type: ignore[attr-defined]
    assert len(call_log) == 1  # lp invoked exactly once via the relay path too

    ack_requests = [(m, p) for m, p in fake_relay.requests if p.endswith("/ack")]
    assert len(ack_requests) == 1
    assert f"/accounts/{ACCOUNT_ID}/approvals/appr-relay-1/ack" in ack_requests[0][1]


def test_poll_once_unreachable_relay_returns_zero(config: Config, db: Database):
    config.relay_url = "http://127.0.0.1:1"  # nothing listens on port 1
    config.relay_account_id = ACCOUNT_ID
    config.relay_account_token = "tok"

    client = RelayClient(config, db)
    assert client.poll_once() == 0  # no exception raised


def test_poll_once_skips_malformed_envelope_without_crashing(relay_config: Config, db: Database, fake_relay):
    job_id = _insert_job(db, relay_config)
    fake_relay.pending_approvals = [
        {"approval_id": "appr-bad", "device_id": DEVICE_ID, "job_id": job_id},  # missing "action"/"created_at"
        {
            "approval_id": "appr-good",
            "device_id": DEVICE_ID,
            "job_id": job_id,
            "action": "keep",
            "created_at": 1,
        },
    ]

    client = RelayClient(relay_config, db)
    applied = client.poll_once()

    # The malformed envelope is logged and skipped (not counted, doesn't
    # raise out of poll_once); the well-formed one right after it is still
    # applied -- one bad envelope from the relay must never wedge the loop.
    assert applied == 1
    ack_paths = [p for _, p in fake_relay.requests if p.endswith("/ack")]
    assert any("appr-good" in p for p in ack_paths)
    assert not any("appr-bad" in p for p in ack_paths)


def test_poll_once_ack_failure_still_counts_applied(relay_config: Config, db: Database, fake_relay, fake_lp_binary):
    job_id = _insert_job(db, relay_config)
    fake_relay.pending_approvals = [
        {
            "approval_id": "appr-relay-ackfail",
            "device_id": DEVICE_ID,
            "job_id": job_id,
            "action": "keep",
            "created_at": 1,
        }
    ]
    fake_relay.ack_should_fail = True

    client = RelayClient(relay_config, db)
    applied = client.poll_once()

    # The approval was still applied locally (idempotently, via the same
    # apply_approval() path as the direct sync API) even though acking it
    # back to the relay failed -- the relay will just re-offer it next
    # poll, and re-applying is a safe no-op (already_applied).
    assert applied == 1
    job = db.get_job(job_id)
    assert job["status"] == "kept"


def test_start_stop_lifecycle(relay_config: Config, db: Database):
    relay_config.relay_poll_interval_seconds = 1
    client = RelayClient(relay_config, db)
    assert client.running is False

    client.start()
    assert client.running is True

    # start() again is a no-op, not a second thread.
    client.start()
    assert client.running is True

    client.stop()
    assert client.running is False


def test_start_is_noop_when_disabled(config: Config, db: Database):
    client = RelayClient(config, db)
    client.start()
    assert client.running is False
