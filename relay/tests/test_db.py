"""Unit tests for relay_server/db.py's lower-level storage operations not
already exercised through test_relay.py's HTTP-level tests -- in
particular prune_delivered_older_than(), which nothing in this repo calls
outside a standalone retention job (see docs/relay.md), so it had zero
test coverage.
"""

import time

from relay_server.db import RelayDatabase

ACCOUNT_ID = "acct-prune-test"


def _record_approval(db: RelayDatabase, approval_id: str) -> None:
    db.create_account(ACCOUNT_ID, "irrelevant-hash", "Household") if db.get_account(ACCOUNT_ID) is None else None
    db.record_approval_if_new(
        approval_id=approval_id,
        account_id=ACCOUNT_ID,
        device_id="dev-1",
        job_id="job-1",
        action="print",
        created_at=1,
    )


def test_record_approval_if_new_is_idempotent(db: RelayDatabase):
    _record_approval(db, "appr-dup")
    is_new = db.record_approval_if_new(
        approval_id="appr-dup",
        account_id=ACCOUNT_ID,
        device_id="dev-1",
        job_id="job-1",
        action="print",
        created_at=1,
    )
    assert is_new is False


def test_prune_only_removes_delivered_approvals_older_than_cutoff(db: RelayDatabase):
    _record_approval(db, "appr-old-undelivered")
    _record_approval(db, "appr-old-delivered")
    _record_approval(db, "appr-recent-delivered")

    now = int(time.time())
    old_cutoff = now - 1000

    with db.transaction() as conn:
        conn.execute(
            "UPDATE approvals SET delivered = 1, delivered_at = ? WHERE approval_id = ?",
            (old_cutoff - 10, "appr-old-delivered"),
        )
        conn.execute(
            "UPDATE approvals SET delivered = 1, delivered_at = ? WHERE approval_id = ?",
            (now, "appr-recent-delivered"),
        )

    removed = db.prune_delivered_older_than(old_cutoff)

    assert removed == 1
    assert db.get_approval("appr-old-delivered") is None
    # Undelivered, however old, is never pruned -- the relay only ever
    # discards approvals it has confirmed delivery for.
    assert db.get_approval("appr-old-undelivered") is not None
    assert db.get_approval("appr-recent-delivered") is not None


def test_mark_delivered_sets_flag_and_timestamp(db: RelayDatabase):
    _record_approval(db, "appr-mark")
    before = db.get_approval("appr-mark")
    assert before["delivered"] == 0
    assert before["delivered_at"] is None

    db.mark_delivered("appr-mark")

    after = db.get_approval("appr-mark")
    assert after["delivered"] == 1
    assert after["delivered_at"] is not None


def test_list_pending_approvals_excludes_delivered(db: RelayDatabase):
    _record_approval(db, "appr-pending")
    _record_approval(db, "appr-delivered")
    db.mark_delivered("appr-delivered")

    pending = db.list_pending_approvals(ACCOUNT_ID)

    assert [row["approval_id"] for row in pending] == ["appr-pending"]
