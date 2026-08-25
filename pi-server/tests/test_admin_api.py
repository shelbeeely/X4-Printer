import base64
import json
import threading
import time
import urllib.error
import urllib.request

import pytest

from xteink_print_server.admin_api import AdminApiServer
from xteink_print_server.config import Config
from xteink_print_server.db import Database
from xteink_print_server.relay_client import RelayClient
from xteink_print_server.util import hash_token, sha256_file

DEVICE_ID = "dev-test1"
DEVICE_TOKEN = "supersecrettoken"
ADMIN_PASSWORD = "let-me-in"


def _insert_job(db: Database, config: Config, title="Doc") -> str:
    xtc_path = config.xtc_dir / "job1.xtc"
    xtc_path.write_bytes(b"XTC" + b"\x00" * 100)
    original_path = config.originals_dir / "job1.pdf"
    original_path.write_bytes(b"%PDF-fake-bytes")
    return db.insert_job(
        title=title,
        source="ipp",
        original_path=str(original_path),
        original_mime="application/pdf",
        original_bytes=original_path.stat().st_size,
        xtc_path=str(xtc_path),
        xtc_bytes=xtc_path.stat().st_size,
        xtc_sha256=sha256_file(xtc_path),
        page_count=1,
    )


@pytest.fixture
def relay(config: Config, db: Database):
    client = RelayClient(config, db)
    yield client
    client.stop()


@pytest.fixture
def running_admin_api(config: Config, db: Database, relay: RelayClient):
    config.admin_host = "127.0.0.1"
    config.admin_port = 0
    config.admin_password = ADMIN_PASSWORD
    server = AdminApiServer(config, db, relay)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    time.sleep(0.05)
    yield f"http://127.0.0.1:{port}/api/admin/v1", db, config
    server.shutdown()
    thread.join(timeout=2)


def _auth_header(password=ADMIN_PASSWORD, user="admin"):
    token = base64.b64encode(f"{user}:{password}".encode()).decode()
    return {"Authorization": f"Basic {token}"}


def _get(url, password=ADMIN_PASSWORD, headers=None):
    h = _auth_header(password) if password is not None else {}
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    return urllib.request.urlopen(req, timeout=5)


def _post(url, body, password=ADMIN_PASSWORD):
    data = json.dumps(body).encode()
    headers = _auth_header(password)
    headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method="POST", headers=headers)
    return urllib.request.urlopen(req, timeout=5)


def test_missing_auth_is_rejected(running_admin_api):
    base, _db, _cfg = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/status", password=None)
    assert exc.value.code == 401
    assert "Basic" in exc.value.headers.get("WWW-Authenticate", "")


def test_wrong_password_is_rejected(running_admin_api):
    base, _db, _cfg = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/status", password="wrong")
    assert exc.value.code == 401


def test_status(running_admin_api):
    base, db, config = running_admin_api
    _insert_job(db, config)
    resp = json.loads(_get(f"{base}/status").read())
    assert resp["jobs_pending"] == 1
    assert resp["device_count"] == 0
    assert resp["relay_enabled"] is False


def test_list_jobs(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config, title="Invoice")
    resp = json.loads(_get(f"{base}/jobs").read())
    assert len(resp["jobs"]) == 1
    job = resp["jobs"][0]
    assert job["job_id"] == job_id
    assert job["title"] == "Invoice"
    assert job["delivered_count"] == 0
    assert job["last_action"] is None


def test_job_action_print_reuses_apply_approval(running_admin_api, fake_lp_binary):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)

    resp = json.loads(_post(f"{base}/jobs/{job_id}/action", {"action": "print"}).read())
    assert resp["status"] == "applied"
    assert resp["detail"] == "printed"

    call_log = fake_lp_binary.log_path.read_text().strip().splitlines()
    assert len(call_log) == 1

    row = db.get_job(job_id)
    assert row["status"] == "kept"


def test_job_action_requeue_clears_deliveries(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    db.mark_delivered(job_id, DEVICE_ID)
    db.set_job_status(job_id, "kept")

    resp = json.loads(_post(f"{base}/jobs/{job_id}/action", {"action": "requeue"}).read())
    assert resp["status"] == "requeued"

    row = db.get_job(job_id)
    assert row["status"] == "pending"
    assert db.list_pending_jobs_for_device(DEVICE_ID) != []


def test_job_action_purge_removes_files_and_row(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    row = db.get_job(job_id)
    from pathlib import Path

    assert Path(row["original_path"]).exists()
    assert Path(row["xtc_path"]).exists()

    resp = json.loads(_post(f"{base}/jobs/{job_id}/action", {"action": "purge"}).read())
    assert resp["status"] == "purged"

    assert db.get_job(job_id) is None
    assert not Path(row["original_path"]).exists()
    assert not Path(row["xtc_path"]).exists()


def test_job_action_unknown_action_rejected(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/jobs/{job_id}/action", {"action": "teleport"})
    assert exc.value.code == 400


def test_devices_list_revoke_rotate(running_admin_api):
    base, db, config = running_admin_api
    db.register_device(DEVICE_ID, hash_token(DEVICE_TOKEN), "Kitchen X4", None)

    resp = json.loads(_get(f"{base}/devices").read())
    assert len(resp["devices"]) == 1
    assert resp["devices"][0]["device_id"] == DEVICE_ID

    rotate_resp = json.loads(_post(f"{base}/devices/{DEVICE_ID}/rotate-token", {}).read())
    assert rotate_resp["status"] == "rotated"
    new_token = rotate_resp["device_token"]
    assert new_token != DEVICE_TOKEN
    row = db.get_device(DEVICE_ID)
    assert row["token_hash"] == hash_token(new_token)

    revoke_resp = json.loads(_post(f"{base}/devices/{DEVICE_ID}/revoke", {}).read())
    assert revoke_resp["status"] == "revoked"
    assert db.get_device(DEVICE_ID) is None


def test_device_action_on_unknown_device_404s(running_admin_api):
    base, _db, _config = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/devices/does-not-exist/revoke", {})
    assert exc.value.code == 404


def test_approvals_list_reflects_admin_action(running_admin_api, fake_lp_binary):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    _post(f"{base}/jobs/{job_id}/action", {"action": "keep"})

    resp = json.loads(_get(f"{base}/approvals").read())
    assert len(resp["approvals"]) == 1
    assert resp["approvals"][0]["received_via"] == "admin"
    assert resp["approvals"][0]["action"] == "keep"


def test_settings_round_trip_and_relay_lifecycle(running_admin_api, relay):
    base, _db, config = running_admin_api

    initial = json.loads(_get(f"{base}/settings").read())
    assert initial["cups_queue"] == config.cups_queue

    updated = json.loads(
        _post(
            f"{base}/settings",
            {
                "cups_queue": "NewQueue",
                "retention_days": 7,
                "relay_url": "https://relay.example.invalid",
                "relay_account_id": "acct-1",
                "relay_account_token": "tok-1",
                "relay_poll_interval_seconds": 5,
                "relay_allow_document_sync": True,
            },
        ).read()
    )
    assert updated["cups_queue"] == "NewQueue"
    assert updated["retention_days"] == 7

    # The same Config instance is shared across every component; a
    # settings POST must mutate it in place, not just write the file.
    assert config.cups_queue == "NewQueue"
    assert config.retention_days == 7
    assert config.relay_url == "https://relay.example.invalid"

    # Enabling relay via settings starts the poll thread.
    assert relay.enabled is True
    assert relay.running is True

    # Persisted to disk so a restart picks it up too.
    saved = json.loads(config.admin_settings_path.read_text())
    assert saved["cups_queue"] == "NewQueue"

    # Disabling it again stops the thread.
    _post(f"{base}/settings", {"relay_url": ""})
    assert relay.enabled is False
    assert relay.running is False


def test_settings_rejects_unknown_field(running_admin_api):
    base, _db, _config = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/settings", {"admin_password": "nice-try"})
    assert exc.value.code == 400


def test_static_index_served(running_admin_api):
    base, _db, _config = running_admin_api
    root = base.rsplit("/api/", 1)[0]
    resp = _get(f"{root}/")
    body = resp.read().decode()
    assert "X4 Print Inbox" in body
    assert resp.headers["Content-Type"].startswith("text/html")
