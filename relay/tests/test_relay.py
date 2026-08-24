import json
import threading
import time
import urllib.error
import urllib.request

import pytest

from relay_server.app import RelayServer
from relay_server.config import RelayConfig
from relay_server.db import RelayDatabase
from relay_server.util import hash_token, new_token

ACCOUNT_ID = "acct-test1"


@pytest.fixture
def account_token(db: RelayDatabase) -> str:
    token = new_token()
    db.create_account(ACCOUNT_ID, hash_token(token), "Test Household")
    return token


@pytest.fixture
def running_relay(config: RelayConfig, db: RelayDatabase, account_token: str):
    config.host = "127.0.0.1"
    config.port = 0
    server = RelayServer(config, db)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    time.sleep(0.05)
    yield f"http://127.0.0.1:{port}/relay/v1", db, account_token
    server.shutdown()
    thread.join(timeout=2)


def _post(url, body, token):
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        url, data=data, method="POST", headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"}
    )
    return urllib.request.urlopen(req, timeout=5)


def _get(url, token):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    return urllib.request.urlopen(req, timeout=5)


def test_submit_and_poll_pending_approval(running_relay):
    base, db, token = running_relay
    body = {
        "approval_id": "appr-1",
        "device_id": "dev-1",
        "job_id": "job-1",
        "action": "print",
        "created_at": 1737590000,
    }
    resp = json.loads(_post(f"{base}/accounts/{ACCOUNT_ID}/approvals", body, token).read())
    assert resp["status"] == "queued"

    pending = json.loads(_get(f"{base}/accounts/{ACCOUNT_ID}/approvals/pending", token).read())
    assert len(pending["approvals"]) == 1
    assert pending["approvals"][0]["approval_id"] == "appr-1"
    assert pending["approvals"][0]["action"] == "print"


def test_duplicate_submit_is_idempotent(running_relay):
    base, db, token = running_relay
    body = {
        "approval_id": "appr-dup",
        "device_id": "dev-1",
        "job_id": "job-1",
        "action": "keep",
        "created_at": 1,
    }
    _post(f"{base}/accounts/{ACCOUNT_ID}/approvals", body, token)
    _post(f"{base}/accounts/{ACCOUNT_ID}/approvals", body, token)

    pending = json.loads(_get(f"{base}/accounts/{ACCOUNT_ID}/approvals/pending", token).read())
    assert len(pending["approvals"]) == 1  # not duplicated


def test_ack_removes_from_pending_and_status_flips(running_relay):
    base, db, token = running_relay
    body = {"approval_id": "appr-ack", "device_id": "dev-1", "job_id": "job-1", "action": "delete", "created_at": 1}
    _post(f"{base}/accounts/{ACCOUNT_ID}/approvals", body, token)

    status_before = json.loads(_get(f"{base}/accounts/{ACCOUNT_ID}/approvals/appr-ack", token).read())
    assert status_before["status"] == "pending"

    ack_resp = json.loads(_post(f"{base}/accounts/{ACCOUNT_ID}/approvals/appr-ack/ack", {}, token).read())
    assert ack_resp["status"] == "ok"

    pending = json.loads(_get(f"{base}/accounts/{ACCOUNT_ID}/approvals/pending", token).read())
    assert pending["approvals"] == []

    status_after = json.loads(_get(f"{base}/accounts/{ACCOUNT_ID}/approvals/appr-ack", token).read())
    assert status_after["status"] == "already_applied"


def test_wrong_token_rejected(running_relay):
    base, db, token = running_relay
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/accounts/{ACCOUNT_ID}/approvals/pending", "wrong-token")
    assert exc.value.code == 401


def test_cross_account_isolation(running_relay, db):
    base, _db, token = running_relay
    other_token = new_token()
    db.create_account("acct-other", hash_token(other_token), "Other Household")

    body = {"approval_id": "appr-iso", "device_id": "dev-1", "job_id": "job-1", "action": "print", "created_at": 1}
    _post(f"{base}/accounts/{ACCOUNT_ID}/approvals", body, token)

    # The other account's token must not see acct-1's pending approvals.
    with pytest.raises(urllib.error.HTTPError) as exc:
        _get(f"{base}/accounts/{ACCOUNT_ID}/approvals/pending", other_token)
    assert exc.value.code == 401
