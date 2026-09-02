"""Runs *inside* the pi-server container (docker-compose.test.yml mounts
this directory to /app/tests_docker) against a real CUPS daemon (docker/cups/)
instead of the fake `lp` every unit/integration test elsewhere in this repo
uses. This is the one place printer_forward.py's actual `lp -d <queue>`
shell-out (docs/architecture.md "CUPS queue is fixed at install time") gets
proven against a real CUPS install rather than a stand-in.

Not part of the default `pytest -q` run anywhere -- it needs a real CUPS
server reachable at $CUPS_SERVER and takes real wall-clock seconds for CUPS
to actually process each job. See docs/testing.md and
.github/workflows/tests.yml's docker-cups-tests job for how this is invoked
(`docker compose -f docker-compose.test.yml run --rm pi-server python -m
pytest tests_docker -q`).
"""

from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

from focusink_server.config import Config
from focusink_server.db import Database
from focusink_server.printer_forward import ApprovalError, apply_approval, submit_to_cups

OUTPUT_DIR = Path("/output")
POLL_TIMEOUT_S = 30


@pytest.fixture
def config(tmp_path: Path) -> Config:
    cfg = Config(
        data_dir=tmp_path / "data",
        cups_queue=os.environ.get("FOCUSINK_CUPS_QUEUE", "PDF"),
        lp_binary=os.environ.get("FOCUSINK_LP_BINARY", "lp"),
    )
    cfg.ensure_dirs()
    return cfg


@pytest.fixture
def db(config: Config) -> Database:
    return Database(config.db_path)


def _insert_job(db: Database, config: Config, title: str, body: bytes) -> str:
    original = config.originals_dir / "orig.txt"
    original.write_bytes(body)
    return db.insert_job(
        title=title,
        source="ipp",
        original_path=str(original),
        original_mime="text/plain",
        original_bytes=original.stat().st_size,
        xtc_path=str(config.xtc_dir / "out.xtc"),
        xtc_bytes=100,
        xtc_sha256="deadbeef",
        page_count=1,
    )


def _wait_for_new_output_file(before: set[str], timeout_s: float = POLL_TIMEOUT_S) -> Path:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        after = {p.name for p in OUTPUT_DIR.iterdir()} if OUTPUT_DIR.is_dir() else set()
        new_files = after - before
        if new_files:
            return OUTPUT_DIR / sorted(new_files)[0]
        time.sleep(0.5)
    raise AssertionError(
        f"no new file appeared under {OUTPUT_DIR} within {timeout_s}s "
        "-- CUPS accepted the job but never finished processing it"
    )


def test_submit_to_cups_produces_a_real_pdf(config: Config, tmp_path: Path):
    before = {p.name for p in OUTPUT_DIR.iterdir()} if OUTPUT_DIR.is_dir() else set()

    doc = tmp_path / "doc.txt"
    doc.write_text("Hello from the Focusink docker CUPS integration test.\n")

    cups_job_id = submit_to_cups(config, str(doc), "Docker CUPS Integration Test")
    assert cups_job_id >= 0

    produced = _wait_for_new_output_file(before)
    data = produced.read_bytes()
    assert len(data) > 0
    # A real queue produces a real PDF, not just a copy of the raw
    # submission -- this is what actually distinguishes this test from the
    # fake-`lp` unit tests elsewhere, which never look at output bytes.
    assert data.startswith(b"%PDF-"), f"expected {produced} to be a PDF, got header {data[:16]!r}"


def test_apply_approval_print_action_reaches_real_cups(config: Config, db: Database, tmp_path: Path):
    before = {p.name for p in OUTPUT_DIR.iterdir()} if OUTPUT_DIR.is_dir() else set()

    job_id = _insert_job(db, config, "Approved Print Job", b"Approved via apply_approval().\n")

    result1 = apply_approval(
        db, config, approval_id="docker-appr-1", device_id="dev-docker", job_id=job_id, action="print", created_at=1
    )
    result2 = apply_approval(
        db, config, approval_id="docker-appr-1", device_id="dev-docker", job_id=job_id, action="print", created_at=1
    )

    assert result1.status == "applied"
    assert result1.detail == "printed"
    assert result1.error is None
    # Idempotency (docs/architecture.md "Idempotent approval application"):
    # a retried approval_id must not submit to CUPS a second time.
    assert result2.status == "already_applied"
    assert result2.cups_job_id == result1.cups_job_id

    produced = _wait_for_new_output_file(before)
    assert produced.read_bytes().startswith(b"%PDF-")

    job = db.get_job(job_id)
    assert job["status"] == "kept"


def test_submit_to_cups_missing_queue_raises_before_touching_lp(tmp_path: Path):
    cfg = Config(data_dir=tmp_path / "data2", cups_queue="", lp_binary=os.environ.get("FOCUSINK_LP_BINARY", "lp"))
    cfg.ensure_dirs()
    doc = tmp_path / "doc2.txt"
    doc.write_text("should never be submitted\n")

    with pytest.raises(ApprovalError):
        submit_to_cups(cfg, str(doc), "Should Not Print")
