from pathlib import Path

from xteink_print_server.config import Config
from xteink_print_server.db import Database
from xteink_print_server.printer_forward import apply_approval, replay_unapplied_approvals


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
