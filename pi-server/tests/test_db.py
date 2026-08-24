from xteink_print_server.db import Database


def _insert_job(db: Database, title="Doc") -> str:
    return db.insert_job(
        title=title,
        source="ipp",
        original_path="/tmp/orig.pdf",
        original_mime="application/pdf",
        original_bytes=1234,
        xtc_path="/tmp/out.xtc",
        xtc_bytes=4321,
        xtc_sha256="deadbeef",
        page_count=2,
    )


def test_insert_and_get_job(db: Database):
    job_id = _insert_job(db)
    row = db.get_job(job_id)
    assert row["title"] == "Doc"
    assert row["status"] == "pending"
    assert row["page_count"] == 2


def test_pending_jobs_hide_delivered(db: Database):
    job_id = _insert_job(db)
    db.register_device("dev-1", "hash", "Test Device", None)

    pending = db.list_pending_jobs_for_device("dev-1")
    assert len(pending) == 1

    db.mark_delivered(job_id, "dev-1")
    pending_after = db.list_pending_jobs_for_device("dev-1")
    assert len(pending_after) == 0

    # A second device hasn't seen it yet.
    pending_other = db.list_pending_jobs_for_device("dev-2")
    assert len(pending_other) == 1


def test_approval_dedup_insert(db: Database):
    job_id = _insert_job(db)
    is_new_1 = db.record_approval_if_new(
        approval_id="appr-1", device_id="dev-1", job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    is_new_2 = db.record_approval_if_new(
        approval_id="appr-1", device_id="dev-1", job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    assert is_new_1 is True
    assert is_new_2 is False

    row = db.get_approval("appr-1")
    assert row["applied"] == 0

    db.mark_approval_applied("appr-1", detail="printed", cups_job_id=7)
    row = db.get_approval("appr-1")
    assert row["applied"] == 1
    assert row["detail"] == "printed"
    assert row["cups_job_id"] == 7


def test_unapplied_approvals_listed_for_replay(db: Database):
    job_id = _insert_job(db)
    db.record_approval_if_new(
        approval_id="appr-x", device_id="dev-1", job_id=job_id, action="keep", created_at=1, received_via="direct"
    )
    unapplied = db.list_unapplied_approvals()
    assert len(unapplied) == 1
    assert unapplied[0]["approval_id"] == "appr-x"

    db.mark_approval_applied("appr-x", detail="kept")
    assert db.list_unapplied_approvals() == []
