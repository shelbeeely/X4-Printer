import json
import threading
import time
import urllib.error
import urllib.request

import pytest

from focusink_server.config import Config
from focusink_server.db import Database
from focusink_server.sync_api import SyncApiServer
from focusink_server.util import hash_token
from tests.conftest import make_test_png


DEVICE_ID = "dev-test1"
DEVICE_TOKEN = "supersecrettoken"


def _insert_job(db: Database, config: Config, title="Doc") -> str:
    xtc_path = config.xtc_dir / "job1.xtc"
    xtc_path.write_bytes(b"XTC" + b"\x00" * 100)
    from focusink_server.util import sha256_file

    original_path = config.originals_dir / "job1.pdf"
    original_path.write_bytes(b"%PDF-fake-bytes")
    job_id, _is_new = db.insert_job(
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
    return job_id


@pytest.fixture
def running_sync_api(config: Config, db: Database):
    db.register_device(DEVICE_ID, hash_token(DEVICE_TOKEN), "Test Device", None)
    config.sync_host = "127.0.0.1"
    config.sync_port = 0
    server = SyncApiServer(config, db)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    time.sleep(0.05)
    yield f"http://127.0.0.1:{port}/api/v1", db, config
    server.shutdown()
    thread.join(timeout=2)


def _get(url, token=DEVICE_TOKEN, device=DEVICE_ID, headers=None):
    h = {"Authorization": f"Bearer {token}", "X-Device-Id": device}
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    return urllib.request.urlopen(req, timeout=5)


def _post(url, body, token=DEVICE_TOKEN, device=DEVICE_ID):
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={"Authorization": f"Bearer {token}", "X-Device-Id": device, "Content-Type": "application/json"},
    )
    return urllib.request.urlopen(req, timeout=5)


def _post_raw(url, data: bytes, content_type: str, token=DEVICE_TOKEN, device=DEVICE_ID):
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={"Authorization": f"Bearer {token}", "X-Device-Id": device, "Content-Type": content_type},
    )
    return urllib.request.urlopen(req, timeout=5)


def test_missing_auth_is_rejected(running_sync_api):
    base, _db, _cfg = running_sync_api
    req = urllib.request.Request(f"{base}/devices/{DEVICE_ID}/jobs")
    with pytest.raises(urllib.error.HTTPError) as exc:
        urllib.request.urlopen(req, timeout=5)
    assert exc.value.code == 401


def test_wrong_token_is_rejected(running_sync_api):
    base, _db, _cfg = running_sync_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/devices/{DEVICE_ID}/jobs", token="wrong")
    assert exc.value.code == 401


def test_device_config_returns_calendars_and_wifi(running_sync_api):
    base, db, _config = running_sync_api
    db.add_calendar_feed("https://example.com/a.ics", "Work")
    db.add_or_update_wifi_network("HomeWiFi", "hunter2")

    resp = json.loads(_get(f"{base}/devices/{DEVICE_ID}/config").read())
    assert resp["calendars"] == [{"url": "https://example.com/a.ics", "label": "Work"}]
    assert resp["wifi_networks"] == [{"ssid": "HomeWiFi", "password": "hunter2"}]
    assert "server_time" in resp


def test_device_config_empty_when_unconfigured(running_sync_api):
    base, _db, _config = running_sync_api
    resp = json.loads(_get(f"{base}/devices/{DEVICE_ID}/config").read())
    assert resp["calendars"] == []
    assert resp["wifi_networks"] == []


def test_device_config_requires_auth(running_sync_api):
    base, _db, _config = running_sync_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/devices/{DEVICE_ID}/config", token="wrong")
    assert exc.value.code == 401


def test_device_config_rejects_device_id_mismatch(running_sync_api):
    base, _db, _config = running_sync_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/devices/dev-other/config")
    assert exc.value.code == 403


def test_list_pending_jobs(running_sync_api):
    base, db, config = running_sync_api
    job_id = _insert_job(db, config)
    resp = _get(f"{base}/devices/{DEVICE_ID}/jobs")
    payload = json.loads(resp.read())
    assert len(payload["jobs"]) == 1
    assert payload["jobs"][0]["job_id"] == job_id


def test_download_xtc_full_and_ranged(running_sync_api):
    base, db, config = running_sync_api
    job_id = _insert_job(db, config)
    row = db.get_job(job_id)

    resp = _get(f"{base}/jobs/{job_id}/xtc")
    body = resp.read()
    assert body == open(row["xtc_path"], "rb").read()
    assert resp.headers["X-Content-SHA256"] == row["xtc_sha256"]

    resp2 = _get(f"{base}/jobs/{job_id}/xtc", headers={"Range": "bytes=0-9"})
    assert resp2.status == 206
    assert len(resp2.read()) == 10


def test_ack_marks_delivered_and_rejects_hash_mismatch(running_sync_api):
    base, db, config = running_sync_api
    job_id = _insert_job(db, config)
    row = db.get_job(job_id)

    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/jobs/{job_id}/ack", {"sha256": "wrong"})
    assert exc.value.code == 409

    resp = _post(f"{base}/jobs/{job_id}/ack", {"sha256": row["xtc_sha256"]})
    assert json.loads(resp.read())["status"] == "ok"

    pending = db.list_pending_jobs_for_device(DEVICE_ID)
    assert pending == []


def test_approval_print_is_idempotent_over_http(running_sync_api, fake_lp_binary):
    base, db, config = running_sync_api
    job_id = _insert_job(db, config)

    body = {
        "approval_id": "appr-http-1",
        "device_id": DEVICE_ID,
        "job_id": job_id,
        "action": "print",
        "created_at": 1737590000,
    }
    resp1 = json.loads(_post(f"{base}/approvals", body).read())
    resp2 = json.loads(_post(f"{base}/approvals", body).read())

    assert resp1["status"] == "applied"
    assert resp1["detail"] == "printed"
    assert resp2["status"] == "already_applied"

    call_log = fake_lp_binary.log_path.read_text().strip().splitlines()
    assert len(call_log) == 1


# -- POST /devices/{id}/jobs/{job_id}: X4 direct-upload endpoint (docs/protocol.md §1.7) --

UPLOAD_JOB_ID = "x4jobfeedfacefeedfacefeedfacefee"


def test_upload_original_requires_auth(running_sync_api):
    base, _db, _config = running_sync_api
    png = make_test_png()
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post_raw(f"{base}/devices/{DEVICE_ID}/jobs/{UPLOAD_JOB_ID}?title=Photo", png, "image/png", token="wrong")
    assert exc.value.code == 401


def test_upload_original_rejects_device_id_mismatch(running_sync_api):
    base, _db, _config = running_sync_api
    png = make_test_png()
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post_raw(f"{base}/devices/dev-other/jobs/{UPLOAD_JOB_ID}?title=Photo", png, "image/png")
    assert exc.value.code == 403


def test_upload_original_rejects_unsupported_mime(running_sync_api):
    base, _db, _config = running_sync_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post_raw(f"{base}/devices/{DEVICE_ID}/jobs/{UPLOAD_JOB_ID}?title=Doc", b"%PDF-fake", "application/pdf")
    assert exc.value.code == 400


def test_upload_original_creates_job_under_device_supplied_id(running_sync_api):
    base, db, _config = running_sync_api
    png = make_test_png()
    resp = json.loads(
        _post_raw(f"{base}/devices/{DEVICE_ID}/jobs/{UPLOAD_JOB_ID}?title=My+Photo", png, "image/png").read()
    )
    assert resp["job_id"] == UPLOAD_JOB_ID
    assert resp["status"] == "created"

    row = db.get_job(UPLOAD_JOB_ID)
    assert row is not None
    assert row["title"] == "My Photo"
    assert row["source"] == "x4_upload"
    assert row["status"] == "pending"


def test_upload_original_retry_with_same_job_id_is_a_no_op(running_sync_api):
    base, db, _config = running_sync_api
    png = make_test_png()
    url = f"{base}/devices/{DEVICE_ID}/jobs/{UPLOAD_JOB_ID}?title=My+Photo"

    resp1 = json.loads(_post_raw(url, png, "image/png").read())
    resp2 = json.loads(_post_raw(url, png, "image/png").read())

    assert resp1["status"] == "created"
    assert resp2["status"] == "already_exists"
    assert resp1["job_id"] == resp2["job_id"] == UPLOAD_JOB_ID
    assert len(db.list_all_jobs()) == 1


def test_approval_device_mismatch_rejected(running_sync_api):
    base, db, config = running_sync_api
    job_id = _insert_job(db, config)
    body = {
        "approval_id": "appr-http-2",
        "device_id": "someone-else",
        "job_id": job_id,
        "action": "keep",
        "created_at": 1,
    }
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/approvals", body)
    assert exc.value.code == 403
