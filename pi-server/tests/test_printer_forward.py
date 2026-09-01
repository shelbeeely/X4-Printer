from pathlib import Path

from focusink_server.config import Config
from focusink_server.db import Database
from focusink_server.printer_forward import apply_approval, replay_unapplied_approvals


def _insert_job(db: Database, config: Config, title="Doc") -> str:
    original = config.originals_dir / "orig.pdf"
    original.write_bytes(b"%PDF-fake")
    job_id, _is_new = db.insert_job(
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
    return job_id


def test_print_approval_calls_lp_exactly_once_even_when_retried(db: Database, config: Config, fake_lp_binary):
    job_id = _insert_job(db, config)

    result1 = apply_approval(
        db, config, approval_id="appr-1", device_id="dev-1", job_id=job_id, action="print", created_at=1
    )
    result2 = apply_approval(
        db, config, approval_id="appr-1", device_id="dev-1", job_id=job_id, action="print", created_at=1
    )

    assert result1.status == "applied"
    assert result1.detail == "printed"
    assert result2.status == "already_applied"
    assert result2.cups_job_id == result1.cups_job_id

    call_log = fake_lp_binary.log_path.read_text().strip().splitlines()  # type: ignore[attr-defined]
    assert len(call_log) == 1  # lp was only ever invoked once

    job = db.get_job(job_id)
    assert job["status"] == "kept"


def test_two_devices_racing_print_for_same_job_prints_exactly_once(db: Database, config: Config, fake_lp_binary):
    """The multi-device gap this feature closes: two DIFFERENT X4s that
    both downloaded the same still-pending job and both tap Print before
    either has synced generate two DIFFERENT approval_ids for the same
    job_id -- record_approval_if_new alone treats both as genuinely new
    (it only dedupes retries of the *same* approval_id). Without
    claim_job_for_finalization, both would reach submit_to_cups(). This is
    the one most worth getting right: assert lp is invoked exactly once."""
    job_id = _insert_job(db, config)

    result_a = apply_approval(
        db, config, approval_id="appr-device-a", device_id="dev-a", job_id=job_id, action="print", created_at=1
    )
    result_b = apply_approval(
        db, config, approval_id="appr-device-b", device_id="dev-b", job_id=job_id, action="print", created_at=2
    )

    assert result_a.status == "applied"
    assert result_a.detail == "printed"
    assert result_b.status == "superseded"
    assert result_b.cups_job_id is None
    assert "appr-device-a" in (result_b.error or "")

    call_log = fake_lp_binary.log_path.read_text().strip().splitlines()  # type: ignore[attr-defined]
    assert len(call_log) == 1  # lp was only ever invoked once, despite two distinct approvals

    job = db.get_job(job_id)
    assert job["status"] == "kept"
    assert job["finalizing_approval_id"] == "appr-device-a"

    # Both approval rows are recorded and marked applied -- device B's
    # outbox can stop retrying this job (see firmware's
    # SyncManager::drainApprovalOutbox(), which treats "superseded" the
    # same as "applied": this job's fate is settled, just not by it).
    approval_b = db.get_approval("appr-device-b")
    assert approval_b["applied"] == 1
    assert approval_b["detail"] == "superseded"


def test_reprint_after_job_already_decided_is_not_superseded(db: Database, config: Config, fake_lp_binary):
    """The finalization claim above is scoped to a job's *first* decision
    (status == "pending"), not "locked forever" -- admin_api.py's job
    action handler intentionally mints a fresh approval_id for every
    action, including reprinting an already-kept/deleted job or changing
    an earlier Keep to a Delete (see test_keep_and_delete_actions_...
    below), and that must keep working after this change."""
    job_id = _insert_job(db, config)

    first = apply_approval(
        db, config, approval_id="appr-first", device_id="dev-1", job_id=job_id, action="print", created_at=1
    )
    assert first.status == "applied"

    reprint = apply_approval(
        db, config, approval_id="appr-reprint", device_id="admin-console", job_id=job_id, action="print",
        created_at=2, received_via="admin",
    )
    assert reprint.status == "applied"
    assert reprint.detail == "printed"

    call_log = fake_lp_binary.log_path.read_text().strip().splitlines()  # type: ignore[attr-defined]
    assert len(call_log) == 2  # both prints, deliberate reprint is not blocked


def test_keep_and_delete_actions_update_status_without_printing(db: Database, config: Config, fake_lp_binary):
    job_id = _insert_job(db, config)

    keep_result = apply_approval(
        db, config, approval_id="appr-keep", device_id="dev-1", job_id=job_id, action="keep", created_at=1
    )
    assert keep_result.detail == "kept"
    assert db.get_job(job_id)["status"] == "kept"

    delete_result = apply_approval(
        db, config, approval_id="appr-delete", device_id="dev-1", job_id=job_id, action="delete", created_at=1
    )
    assert delete_result.detail == "archived"
    assert db.get_job(job_id)["status"] == "deleted"

    assert not fake_lp_binary.log_path.exists()  # type: ignore[attr-defined]


def test_print_with_no_cups_queue_configured_records_failure_not_crash(db: Database, tmp_path):
    cfg = Config(
        data_dir=tmp_path / "data2",
        cups_queue="",
        lp_binary="lp",
        tls_cert=tmp_path / "data2" / "tls" / "server.crt",
        tls_key=tmp_path / "data2" / "tls" / "server.key",
    )
    cfg.ensure_dirs()
    job_id = _insert_job(db, cfg)

    result = apply_approval(db, cfg, approval_id="appr-fail", device_id="dev-1", job_id=job_id, action="print", created_at=1)
    assert result.status == "applied"
    assert result.detail == "print_failed"
    assert result.error is not None

    row = db.get_approval("appr-fail")
    assert row["applied"] == 1
    assert row["error"] is not None


def test_replay_unapplied_approvals_finishes_interrupted_apply(db: Database, config: Config, fake_lp_binary):
    job_id = _insert_job(db, config)
    # Simulate a crash between record and apply: insert the row directly,
    # skipping the side effect, the way apply_approval's first half would
    # leave things if the process died right there.
    db.record_approval_if_new(
        approval_id="appr-crash", device_id="dev-1", job_id=job_id, action="print", created_at=1, received_via="direct"
    )
    assert db.get_approval("appr-crash")["applied"] == 0

    replayed = replay_unapplied_approvals(db, config)
    assert replayed == 1
    row = db.get_approval("appr-crash")
    assert row["applied"] == 1
    assert row["detail"] == "printed"


def test_invalid_action_is_rejected_without_touching_db(db: Database, config: Config):
    job_id = _insert_job(db, config)
    result = apply_approval(db, config, approval_id="appr-bad", device_id="dev-1", job_id=job_id, action="explode", created_at=1)
    assert result.status == "rejected"
    assert db.get_approval("appr-bad") is None
