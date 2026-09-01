"""Idempotent approval application: the single code path that turns an
approval envelope (from the direct sync API or the relay) into either a
CUPS print job or a job-status update.

See docs/architecture.md "Idempotent approval application" for the
transaction shape this implements: db.record_approval_if_new() and
db.mark_approval_applied() bracket the side effect so a duplicate
approval_id (retry, or the same approval arriving via both the direct path
and the relay) never repeats the side effect.
"""

from __future__ import annotations

import logging
import re
import subprocess
from dataclasses import dataclass
from typing import Optional

from .config import Config
from .db import Database

logger = logging.getLogger("focusink.printer_forward")

VALID_ACTIONS = {"print", "keep", "delete"}

_LP_JOB_ID_RE = re.compile(r"request id is\s+\S+-(\d+)", re.IGNORECASE)


@dataclass
class ApprovalResult:
    approval_id: str
    status: str  # 'applied' | 'already_applied' | 'rejected'
    detail: str
    cups_job_id: Optional[int] = None
    error: Optional[str] = None


class ApprovalError(Exception):
    pass


def submit_to_cups(config: Config, original_path: str, title: str) -> int:
    """Shells out to `lp -d <configured queue> <file>`. Only ever uses the
    queue name from server config — never a value supplied by the request —
    so an approval payload can't redirect output to an arbitrary CUPS
    destination."""
    if not config.cups_queue:
        raise ApprovalError("no CUPS queue configured (FOCUSINK_CUPS_QUEUE); cannot forward print job")

    cmd = [config.lp_binary, "-d", config.cups_queue, "-t", title, original_path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60, check=False)
    except FileNotFoundError as exc:
        raise ApprovalError(f"'{config.lp_binary}' is not installed or not on PATH") from exc
    except subprocess.TimeoutExpired as exc:
        raise ApprovalError("lp command timed out") from exc

    if proc.returncode != 0:
        raise ApprovalError(f"lp failed (exit {proc.returncode}): {proc.stderr.strip() or proc.stdout.strip()}")

    match = _LP_JOB_ID_RE.search(proc.stdout)
    cups_job_id = int(match.group(1)) if match else 0
    logger.info("submitted %s to CUPS queue %s (cups job id=%s)", original_path, config.cups_queue, cups_job_id)
    return cups_job_id


def _apply_side_effect(db: Database, config: Config, *, job_id: str, action: str) -> tuple[str, Optional[int]]:
    """Performs the actual effect for a brand-new approval. Returns (detail, cups_job_id)."""
    job = db.get_job(job_id)
    if job is None:
        raise ApprovalError(f"unknown job_id {job_id!r}")

    if action == "print":
        cups_job_id = submit_to_cups(config, job["original_path"], job["title"])
        if job["status"] != "deleted":
            db.set_job_status(job_id, "kept")
        return "printed", cups_job_id
    if action == "keep":
        db.set_job_status(job_id, "kept")
        return "kept", None
    if action == "delete":
        db.set_job_status(job_id, "deleted")
        return "archived", None
    raise ApprovalError(f"unknown action {action!r}")


def apply_approval(
    db: Database,
    config: Config,
    *,
    approval_id: str,
    device_id: str,
    job_id: str,
    action: str,
    created_at: int,
    received_via: str = "direct",
) -> ApprovalResult:
    if action not in VALID_ACTIONS:
        return ApprovalResult(approval_id, "rejected", "invalid_action", error=f"unknown action {action!r}")

    is_new = db.record_approval_if_new(
        approval_id=approval_id,
        device_id=device_id,
        job_id=job_id,
        action=action,
        created_at=created_at,
        received_via=received_via,
    )

    if not is_new:
        existing = db.get_approval(approval_id)
        if existing is None:  # pragma: no cover - defensive, can't happen under the write lock
            raise ApprovalError("approval vanished after dedup check")
        if not existing["applied"]:
            # Process died between record and apply on a previous attempt;
            # replay the side effect exactly once now (see
            # replay_unapplied_approvals below, invoked at startup — this
            # branch is the same replay logic reached synchronously if a
            # retry lands here first).
            return _finish(db, approval_id, job_id, action, config)
        return ApprovalResult(
            approval_id,
            "already_applied",
            existing["detail"] or "",
            cups_job_id=existing["cups_job_id"],
            error=existing["error"],
        )

    return _finish(db, approval_id, job_id, action, config)


def _finish(db: Database, approval_id: str, job_id: str, action: str, config: Config) -> ApprovalResult:
    try:
        detail, cups_job_id = _apply_side_effect(db, config, job_id=job_id, action=action)
        db.mark_approval_applied(approval_id, detail=detail, cups_job_id=cups_job_id)
        return ApprovalResult(approval_id, "applied", detail, cups_job_id=cups_job_id)
    except ApprovalError as exc:
        detail = "print_failed" if action == "print" else "failed"
        db.mark_approval_applied(approval_id, detail=detail, error=str(exc))
        logger.error("approval %s failed: %s", approval_id, exc)
        return ApprovalResult(approval_id, "applied", detail, error=str(exc))


def replay_unapplied_approvals(db: Database, config: Config) -> int:
    """Called once at server startup: finishes any approval whose row was
    recorded but whose side effect never completed (process died in
    between). Returns the count replayed."""
    pending = db.list_unapplied_approvals()
    for row in pending:
        logger.warning("replaying unapplied approval %s (job=%s action=%s)", row["approval_id"], row["job_id"], row["action"])
        _finish(db, row["approval_id"], row["job_id"], row["action"], config)
    return len(pending)
