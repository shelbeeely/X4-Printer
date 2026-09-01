import base64
import json
import threading
import time
import urllib.error
import urllib.request

import pytest

from focusink_server.admin_api import AdminApiServer
from focusink_server.config import Config
from focusink_server.db import Database
from focusink_server.relay_client import RelayClient
from focusink_server.util import hash_token, sha256_file

DEVICE_ID = "dev-test1"
DEVICE_TOKEN = "supersecrettoken"
ADMIN_PASSWORD = "let-me-in"


def _insert_job(db: Database, config: Config, title="Doc", with_thumbnail=False, with_landscape=False) -> str:
    xtc_path = config.xtc_dir / "job1.xtc"
    xtc_path.write_bytes(b"XTC" + b"\x00" * 100)
    original_path = config.originals_dir / "job1.pdf"
    original_path.write_bytes(b"%PDF-fake-bytes")
    thumbnail_path = ""
    if with_thumbnail:
        thumb = config.thumbnails_dir / "job1.jpg"
        thumb.write_bytes(b"\xff\xd8\xff-fake-jpeg-bytes")
        thumbnail_path = str(thumb)
    xtc_landscape_path = ""
    xtc_landscape_bytes = 0
    xtc_landscape_sha256 = ""
    if with_landscape:
        landscape_path = config.xtc_dir / "job1_landscape.xtc"
        landscape_path.write_bytes(b"XTC" + b"\x00" * 200)
        xtc_landscape_path = str(landscape_path)
        xtc_landscape_bytes = landscape_path.stat().st_size
        xtc_landscape_sha256 = sha256_file(landscape_path)
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
        thumbnail_path=thumbnail_path,
        xtc_landscape_path=xtc_landscape_path,
        xtc_landscape_bytes=xtc_landscape_bytes,
        xtc_landscape_sha256=xtc_landscape_sha256,
        xtc_landscape_page_count=2 if with_landscape else 0,
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


def test_get_original_streams_untouched_file(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)

    resp = _get(f"{base}/jobs/{job_id}/original")
    assert resp.headers["Content-Type"] == "application/pdf"
    assert resp.read() == b"%PDF-fake-bytes"


def test_get_original_requires_auth(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/jobs/{job_id}/original", password=None)
    assert exc.value.code == 401


def test_get_original_unknown_job_404s(running_admin_api):
    base, _db, _config = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/jobs/does-not-exist/original")
    assert exc.value.code == 404


def test_get_original_missing_file_404s(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    row = db.get_job(job_id)
    from pathlib import Path

    Path(row["original_path"]).unlink()
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/jobs/{job_id}/original")
    assert exc.value.code == 404


def test_get_thumbnail_streams_jpeg(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config, with_thumbnail=True)

    resp = _get(f"{base}/jobs/{job_id}/thumbnail")
    assert resp.headers["Content-Type"] == "image/jpeg"
    assert resp.read() == b"\xff\xd8\xff-fake-jpeg-bytes"


def test_get_thumbnail_no_thumbnail_404s(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config, with_thumbnail=False)
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/jobs/{job_id}/thumbnail")
    assert exc.value.code == 404


def test_get_thumbnail_unknown_job_404s(running_admin_api):
    base, _db, _config = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/jobs/does-not-exist/thumbnail")
    assert exc.value.code == 404


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
    # No thumbnail on this job (default) — thumbnail_path is '' (the
    # migration default), which regression-tests the purge guard: an
    # earlier version unconditionally called Path(job[field]).unlink() for
    # every field, and Path("") resolves to the current working directory,
    # which unlink() refuses with IsADirectoryError regardless of
    # missing_ok. This test failing with that error is exactly what the
    # guard in admin_api.py's purge branch prevents.
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


def test_job_action_purge_removes_thumbnail_when_present(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config, with_thumbnail=True)
    row = db.get_job(job_id)
    from pathlib import Path

    assert Path(row["thumbnail_path"]).exists()

    resp = json.loads(_post(f"{base}/jobs/{job_id}/action", {"action": "purge"}).read())
    assert resp["status"] == "purged"
    assert not Path(row["thumbnail_path"]).exists()


def test_job_action_purge_removes_landscape_variant_when_present(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config, with_landscape=True)
    row = db.get_job(job_id)
    from pathlib import Path

    assert Path(row["xtc_landscape_path"]).exists()

    resp = json.loads(_post(f"{base}/jobs/{job_id}/action", {"action": "purge"}).read())
    assert resp["status"] == "purged"
    assert not Path(row["xtc_landscape_path"]).exists()


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


def test_device_approvals_scoped_to_one_device(running_admin_api):
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    db.record_approval_if_new(
        approval_id="a1", device_id=DEVICE_ID, job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    db.record_approval_if_new(
        approval_id="a2", device_id="dev-other", job_id=job_id, action="keep", created_at=2, received_via="direct"
    )

    resp = json.loads(_get(f"{base}/devices/{DEVICE_ID}/approvals").read())
    assert len(resp["approvals"]) == 1
    assert resp["approvals"][0]["approval_id"] == "a1"


def test_device_approvals_cors_headers_reflect_origin(running_admin_api):
    # The X4 page's fetch(url, {credentials: "include"}) needs the actual
    # Origin reflected back (never "*", which browsers reject for
    # credentialed requests) plus Allow-Credentials, on the real GET.
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    db.record_approval_if_new(
        approval_id="a1", device_id=DEVICE_ID, job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    origin = "http://x4-device.local:80"

    resp = _get(f"{base}/devices/{DEVICE_ID}/approvals", headers={"Origin": origin})
    assert resp.headers["Access-Control-Allow-Origin"] == origin
    assert resp.headers["Access-Control-Allow-Credentials"] == "true"
    assert json.loads(resp.read())["approvals"][0]["approval_id"] == "a1"


def test_device_approvals_other_routes_have_no_cors_headers(running_admin_api):
    # CORS is scoped to devices/{id}/approvals only — original/thumbnail
    # are loaded via <a>/<img>, which never trigger CORS enforcement, so no
    # headers are needed or wanted there.
    base, db, config = running_admin_api
    job_id = _insert_job(db, config)
    resp = _get(f"{base}/jobs/{job_id}/original", headers={"Origin": "http://x4-device.local"})
    assert "Access-Control-Allow-Origin" not in resp.headers


def test_device_approvals_preflight_options(running_admin_api):
    base, _db, _config = running_admin_api
    origin = "http://x4-device.local:80"
    req = urllib.request.Request(
        f"{base}/devices/{DEVICE_ID}/approvals", method="OPTIONS", headers={"Origin": origin}
    )
    resp = urllib.request.urlopen(req, timeout=5)
    assert resp.status == 204
    assert resp.headers["Access-Control-Allow-Origin"] == origin
    assert resp.headers["Access-Control-Allow-Credentials"] == "true"
    assert resp.headers["Access-Control-Allow-Methods"] == "GET"
    assert resp.headers["Access-Control-Allow-Headers"] == "Authorization"


def test_preflight_options_does_not_require_auth(running_admin_api):
    # Preflight requests never carry credentials — that's expected, not a
    # bug — so do_OPTIONS must not gate on _authenticate().
    base, _db, _config = running_admin_api
    req = urllib.request.Request(
        f"{base}/devices/{DEVICE_ID}/approvals", method="OPTIONS", headers={"Origin": "http://x4-device.local"}
    )
    resp = urllib.request.urlopen(req, timeout=5)  # no Authorization header at all
    assert resp.status == 204


def test_preflight_options_unmatched_path_404s(running_admin_api):
    base, _db, _config = running_admin_api
    req = urllib.request.Request(f"{base}/jobs", method="OPTIONS", headers={"Origin": "http://x4-device.local"})
    with pytest.raises(urllib.error.HTTPError) as exc:
        urllib.request.urlopen(req, timeout=5)
    assert exc.value.code == 404


def test_device_approvals_unknown_device_returns_empty_not_404(running_admin_api):
    # Unlike revoke/rotate-token, this route doesn't check "device exists"
    # first — a revoked device's history is still valid history to show.
    base, _db, _config = running_admin_api
    resp = json.loads(_get(f"{base}/devices/never-paired/approvals").read())
    assert resp["approvals"] == []


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
    assert "Focusink" in body
    assert resp.headers["Content-Type"].startswith("text/html")


# -- calendars & Wi-Fi (synced to every device, docs/protocol.md §1.6) -------


def test_add_and_list_calendars(running_admin_api):
    base, _db, _config = running_admin_api
    resp = json.loads(_post(f"{base}/calendars", {"url": "https://example.com/a.ics", "label": "Work"}).read())
    assert "id" in resp

    listed = json.loads(_get(f"{base}/calendars").read())
    assert len(listed["calendars"]) == 1
    assert listed["calendars"][0]["url"] == "https://example.com/a.ics"
    assert listed["calendars"][0]["label"] == "Work"
    assert listed["max"] == 4


def test_add_calendar_requires_url(running_admin_api):
    base, _db, _config = running_admin_api
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/calendars", {"label": "No URL"})
    assert exc.value.code == 400


def test_add_calendar_enforces_max(running_admin_api):
    base, _db, _config = running_admin_api
    for i in range(4):
        _post(f"{base}/calendars", {"url": f"https://example.com/{i}.ics", "label": ""})
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/calendars", {"url": "https://example.com/one-too-many.ics", "label": ""})
    assert exc.value.code == 400


def test_delete_calendar(running_admin_api):
    base, _db, _config = running_admin_api
    added = json.loads(_post(f"{base}/calendars", {"url": "https://example.com/a.ics", "label": ""}).read())
    _post(f"{base}/calendars/{added['id']}/delete", {})
    listed = json.loads(_get(f"{base}/calendars").read())
    assert listed["calendars"] == []


def test_add_and_list_wifi_networks(running_admin_api):
    base, _db, _config = running_admin_api
    resp = json.loads(_post(f"{base}/wifi-networks", {"ssid": "HomeWiFi", "password": "hunter2"}).read())
    assert "id" in resp

    listed = json.loads(_get(f"{base}/wifi-networks").read())
    assert len(listed["wifi_networks"]) == 1
    assert listed["wifi_networks"][0]["ssid"] == "HomeWiFi"
    assert listed["wifi_networks"][0]["password"] == "hunter2"
    assert listed["max"] == 8


def test_add_wifi_network_upserts_existing_ssid(running_admin_api):
    base, _db, _config = running_admin_api
    first = json.loads(_post(f"{base}/wifi-networks", {"ssid": "HomeWiFi", "password": "old"}).read())
    second = json.loads(_post(f"{base}/wifi-networks", {"ssid": "HomeWiFi", "password": "new"}).read())
    assert first["id"] == second["id"]
    listed = json.loads(_get(f"{base}/wifi-networks").read())
    assert len(listed["wifi_networks"]) == 1
    assert listed["wifi_networks"][0]["password"] == "new"


def test_add_wifi_network_enforces_max(running_admin_api):
    base, _db, _config = running_admin_api
    for i in range(8):
        _post(f"{base}/wifi-networks", {"ssid": f"Net{i}", "password": "pw"})
    with pytest.raises(urllib.error.HTTPError) as exc:
        _post(f"{base}/wifi-networks", {"ssid": "OneTooMany", "password": "pw"})
    assert exc.value.code == 400


def test_delete_wifi_network(running_admin_api):
    base, _db, _config = running_admin_api
    added = json.loads(_post(f"{base}/wifi-networks", {"ssid": "HomeWiFi", "password": "hunter2"}).read())
    _post(f"{base}/wifi-networks/{added['id']}/delete", {})
    listed = json.loads(_get(f"{base}/wifi-networks").read())
    assert listed["wifi_networks"] == []
