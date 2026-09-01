"""SQLite-backed durable state: jobs, per-device deliveries, approvals, devices.

Single-file design (see docs/architecture.md "Data model") so the whole
server's durable state is one file to back up. WAL mode lets the IPP
listener, sync API, and relay-poller threads share one database file
without lock contention on ordinary reads.

Idempotent approval application (docs/protocol.md §3) lives here as
``record_approval``/``mark_approval_applied`` rather than in the callers, so
every caller (direct sync API, relay-drained approvals, and the admin web
console's reprint/keep/archive actions — see ``admin_api.py``) gets the
same atomicity guarantee for free.
"""

from __future__ import annotations

import contextlib
import sqlite3
import threading
import time
import uuid
from pathlib import Path
from typing import Iterator, Optional

SCHEMA = """
CREATE TABLE IF NOT EXISTS jobs (
    job_id TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    source TEXT NOT NULL DEFAULT 'ipp',
    original_path TEXT NOT NULL,
    original_mime TEXT NOT NULL,
    original_bytes INTEGER NOT NULL,
    xtc_path TEXT NOT NULL,
    xtc_bytes INTEGER NOT NULL,
    xtc_sha256 TEXT NOT NULL,
    page_count INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    thumbnail_path TEXT NOT NULL DEFAULT '',
    xtc_landscape_path TEXT NOT NULL DEFAULT '',
    xtc_landscape_bytes INTEGER NOT NULL DEFAULT 0,
    xtc_landscape_sha256 TEXT NOT NULL DEFAULT '',
    xtc_landscape_page_count INTEGER NOT NULL DEFAULT 0,
    -- Set by claim_job_for_finalization() to the device-originated print
    -- approval_id that "owns" this job's print outcome, closing the
    -- multi-device duplicate-print gap: two different devices'
    -- independent print approvals for the same job_id must never both
    -- invoke submit_to_cups(). See printer_forward._finish() and
    -- docs/architecture.md "Idempotent approval application".
    finalizing_approval_id TEXT
);

CREATE TABLE IF NOT EXISTS job_deliveries (
    job_id TEXT NOT NULL,
    device_id TEXT NOT NULL,
    delivered_at INTEGER NOT NULL,
    PRIMARY KEY (job_id, device_id)
);

CREATE TABLE IF NOT EXISTS approvals (
    approval_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    job_id TEXT NOT NULL,
    action TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    received_at INTEGER NOT NULL,
    received_via TEXT NOT NULL DEFAULT 'direct',
    applied INTEGER NOT NULL DEFAULT 0,
    applied_at INTEGER,
    detail TEXT,
    cups_job_id INTEGER,
    error TEXT
);

CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY,
    token_hash TEXT NOT NULL,
    name TEXT NOT NULL,
    account_id TEXT,
    paired_at INTEGER NOT NULL,
    last_seen_at INTEGER
);

-- Household-wide (not per-device), managed from the admin console and
-- pushed to every paired device via GET /devices/{id}/config
-- (docs/protocol.md §1.6) -- see sync_api.py's _handle_device_config.
-- `position` is the display/priority order the admin console lets you
-- drag-reorder; the device only ever keeps the first N (firmware's
-- config::kMaxCalendars / kMaxWifiNetworks -- see admin_api.py's
-- MAX_CALENDAR_FEEDS/MAX_WIFI_NETWORKS, kept in sync with those constants
-- by hand since there's no shared cross-language header in this repo).
CREATE TABLE IF NOT EXISTS calendar_feeds (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    url TEXT NOT NULL,
    label TEXT NOT NULL DEFAULT '',
    position INTEGER NOT NULL
);

-- UNIQUE(ssid): add_or_update_wifi_network is an upsert keyed on ssid, same
-- "insert or update, never duplicate" shape as firmware's own
-- WifiStore::addOrUpdate (config/WifiStore.h) that the device applies this
-- list through on sync -- see that header for why this is a merge, not a
-- wholesale replace (a Pi-side list missing the network the device is
-- currently on must never strand it).
CREATE TABLE IF NOT EXISTS wifi_networks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ssid TEXT NOT NULL UNIQUE,
    password TEXT NOT NULL,
    position INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_jobs_status ON jobs(status);
CREATE INDEX IF NOT EXISTS idx_approvals_device ON approvals(device_id);
"""


def _ensure_column(conn: sqlite3.Connection, table: str, column: str, decl: str) -> None:
    """Adds `column` to `table` if it isn't already there. This project's
    SCHEMA is CREATE TABLE IF NOT EXISTS, executed once — a no-op against an
    already-existing jobs.db from a prior install, so a genuinely new
    column (like jobs.thumbnail_path below) needs an explicit, idempotent
    migration rather than just editing SCHEMA and hoping. This only covers
    additive, nullable columns; changing a column's type or dropping one
    would need a real migration story this project doesn't have yet."""
    cols = {row[1] for row in conn.execute(f"PRAGMA table_info({table})")}
    if column not in cols:
        conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {decl}")


class Database:
    def __init__(self, path: Path):
        self.path = path
        self._local = threading.local()
        self._write_lock = threading.Lock()
        with self._connect() as conn:
            conn.executescript(SCHEMA)
            _ensure_column(conn, "jobs", "thumbnail_path", "TEXT NOT NULL DEFAULT ''")
            _ensure_column(conn, "jobs", "xtc_landscape_path", "TEXT NOT NULL DEFAULT ''")
            _ensure_column(conn, "jobs", "xtc_landscape_bytes", "INTEGER NOT NULL DEFAULT 0")
            _ensure_column(conn, "jobs", "xtc_landscape_sha256", "TEXT NOT NULL DEFAULT ''")
            _ensure_column(conn, "jobs", "xtc_landscape_page_count", "INTEGER NOT NULL DEFAULT 0")
            _ensure_column(conn, "jobs", "finalizing_approval_id", "TEXT")

    def _connect(self) -> sqlite3.Connection:
        if not hasattr(self._local, "conn"):
            conn = sqlite3.connect(str(self.path), timeout=30, check_same_thread=False)
            conn.row_factory = sqlite3.Row
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA foreign_keys=ON")
            conn.execute("PRAGMA busy_timeout=30000")
            self._local.conn = conn
        return self._local.conn

    @contextlib.contextmanager
    def transaction(self) -> Iterator[sqlite3.Connection]:
        """Serialize writers across threads with a real lock, then run a
        SQLite transaction. A Pi Zero has one CPU core and this server's
        write volume is print-job scale (not request-per-millisecond), so a
        single global write lock is simpler and safer than fine-grained
        per-table locking, and avoids SQLITE_BUSY retries entirely."""
        with self._write_lock:
            conn = self._connect()
            conn.execute("BEGIN IMMEDIATE")
            try:
                yield conn
                conn.commit()
            except Exception:
                conn.rollback()
                raise

    def query(self, sql: str, params: tuple = ()) -> list[sqlite3.Row]:
        conn = self._connect()
        return conn.execute(sql, params).fetchall()

    def query_one(self, sql: str, params: tuple = ()) -> Optional[sqlite3.Row]:
        conn = self._connect()
        return conn.execute(sql, params).fetchone()

    # -- Jobs -----------------------------------------------------------

    def insert_job(
        self,
        *,
        title: str,
        source: str,
        original_path: str,
        original_mime: str,
        original_bytes: int,
        xtc_path: str,
        xtc_bytes: int,
        xtc_sha256: str,
        page_count: int,
        thumbnail_path: str = "",
        xtc_landscape_path: str = "",
        xtc_landscape_bytes: int = 0,
        xtc_landscape_sha256: str = "",
        xtc_landscape_page_count: int = 0,
        job_id: Optional[str] = None,
    ) -> tuple[str, bool]:
        """Inserts a new job row. `job_id` defaults to a fresh uuid4 (the
        IPP path's existing behavior, unchanged) — pass an explicit one for
        an X4-generated id (convert.ingest_document's x4_upload source), so
        a retried upload (dropped connection after the Pi already ingested
        it) becomes a cheap no-op via INSERT OR IGNORE rather than a
        duplicate row. Returns (job_id, is_new) — same shape as
        record_approval_if_new's own idempotency pattern; is_new is always
        True when job_id is None, since a fresh uuid4 never collides."""
        resolved_id = job_id or uuid.uuid4().hex
        with self.transaction() as conn:
            cur = conn.execute(
                """INSERT OR IGNORE INTO jobs
                   (job_id, title, created_at, source, original_path, original_mime,
                    original_bytes, xtc_path, xtc_bytes, xtc_sha256, page_count, status, thumbnail_path,
                    xtc_landscape_path, xtc_landscape_bytes, xtc_landscape_sha256, xtc_landscape_page_count)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'pending', ?, ?, ?, ?, ?)""",
                (
                    resolved_id,
                    title,
                    int(time.time()),
                    source,
                    original_path,
                    original_mime,
                    original_bytes,
                    xtc_path,
                    xtc_bytes,
                    xtc_sha256,
                    page_count,
                    thumbnail_path,
                    xtc_landscape_path,
                    xtc_landscape_bytes,
                    xtc_landscape_sha256,
                    xtc_landscape_page_count,
                ),
            )
            return resolved_id, cur.rowcount == 1

    def get_job(self, job_id: str) -> Optional[sqlite3.Row]:
        return self.query_one("SELECT *, rowid AS ipp_job_id FROM jobs WHERE job_id = ?", (job_id,))

    def get_job_by_ipp_id(self, ipp_job_id: int) -> Optional[sqlite3.Row]:
        """jobs.job_id (uuid) is the durable identity used everywhere else;
        IPP itself needs small integers, so the sqlite rowid (implicit,
        stable for the row's lifetime since jobs are never re-inserted)
        doubles as the ipp job-id with no extra column."""
        return self.query_one("SELECT *, rowid AS ipp_job_id FROM jobs WHERE rowid = ?", (ipp_job_id,))

    def list_pending_jobs_for_device(self, device_id: str) -> list[sqlite3.Row]:
        return self.query(
            """SELECT j.* FROM jobs j
               WHERE j.status = 'pending'
                 AND NOT EXISTS (
                   SELECT 1 FROM job_deliveries d
                   WHERE d.job_id = j.job_id AND d.device_id = ?
                 )
               ORDER BY j.created_at ASC""",
            (device_id,),
        )

    def list_all_jobs(self) -> list[sqlite3.Row]:
        return self.query("SELECT * FROM jobs ORDER BY created_at ASC")

    def mark_delivered(self, job_id: str, device_id: str) -> None:
        with self.transaction() as conn:
            conn.execute(
                "INSERT OR REPLACE INTO job_deliveries (job_id, device_id, delivered_at) VALUES (?, ?, ?)",
                (job_id, device_id, int(time.time())),
            )

    def set_job_status(self, job_id: str, status: str) -> None:
        with self.transaction() as conn:
            conn.execute("UPDATE jobs SET status = ? WHERE job_id = ?", (status, job_id))

    # -- Devices ----------------------------------------------------------

    def register_device(self, device_id: str, token_hash: str, name: str, account_id: str | None) -> None:
        with self.transaction() as conn:
            conn.execute(
                """INSERT INTO devices (device_id, token_hash, name, account_id, paired_at)
                   VALUES (?, ?, ?, ?, ?)
                   ON CONFLICT(device_id) DO UPDATE SET
                     token_hash=excluded.token_hash, name=excluded.name, account_id=excluded.account_id""",
                (device_id, token_hash, name, account_id, int(time.time())),
            )

    def get_device(self, device_id: str) -> Optional[sqlite3.Row]:
        return self.query_one("SELECT * FROM devices WHERE device_id = ?", (device_id,))

    def touch_device(self, device_id: str) -> None:
        with self.transaction() as conn:
            conn.execute("UPDATE devices SET last_seen_at = ? WHERE device_id = ?", (int(time.time()), device_id))

    # -- Approvals (idempotent apply) --------------------------------------

    def get_approval(self, approval_id: str) -> Optional[sqlite3.Row]:
        return self.query_one("SELECT * FROM approvals WHERE approval_id = ?", (approval_id,))

    def record_approval_if_new(
        self,
        *,
        approval_id: str,
        device_id: str,
        job_id: str,
        action: str,
        created_at: int,
        received_via: str,
    ) -> bool:
        """Insert the approval row if it doesn't already exist. Returns True
        if this call created the row (caller should now apply the side
        effect), False if it already existed (caller must not repeat the
        side effect — read the existing row's result instead)."""
        with self.transaction() as conn:
            cur = conn.execute(
                """INSERT OR IGNORE INTO approvals
                   (approval_id, device_id, job_id, action, created_at, received_at, received_via, applied)
                   VALUES (?, ?, ?, ?, ?, ?, ?, 0)""",
                (approval_id, device_id, job_id, action, created_at, int(time.time()), received_via),
            )
            return cur.rowcount == 1

    def mark_approval_applied(
        self,
        approval_id: str,
        *,
        detail: str,
        cups_job_id: Optional[int] = None,
        error: Optional[str] = None,
    ) -> None:
        with self.transaction() as conn:
            conn.execute(
                """UPDATE approvals SET applied=1, applied_at=?, detail=?, cups_job_id=?, error=?
                   WHERE approval_id=?""",
                (int(time.time()), detail, cups_job_id, error, approval_id),
            )

    def claim_job_for_finalization(self, job_id: str, approval_id: str) -> bool:
        """Atomically claims job_id's finalized outcome for approval_id —
        the fix for a real gap record_approval_if_new alone doesn't cover:
        it only dedupes *retries of the same approval_id*, not two
        *different* devices independently approving the same job (each
        gets its own fresh approval_id, so record_approval_if_new treats
        both as genuinely new). See printer_forward._finish() for the
        policy on which approvals this is even consulted for (action ==
        "print" from a device, not the admin console, and not
        keep/delete, which have no irreversible side effect to protect).

        This is its own transaction() call, not inlined into the approval
        row's own INSERT OR IGNORE — safe because Database.transaction()
        serializes every writer through self._write_lock for its whole
        duration, so no other transaction (including another call to this
        method) can interleave between them; the atomic UPDATE below is
        what actually closes the TOCTOU gap a naive "check then apply"
        would have, since the side effect itself (submit_to_cups) can't
        run inside a DB transaction.

        The `OR finalizing_approval_id = ?` clause is what makes a *retry
        of the same* approval_id still succeed (crash-replay case, see
        replay_unapplied_approvals) while a genuinely different
        approval_id for the same job correctly fails the claim. Returns
        True if this approval_id now owns (or already owned) the job."""
        with self.transaction() as conn:
            cur = conn.execute(
                """UPDATE jobs SET finalizing_approval_id = ?
                   WHERE job_id = ? AND (finalizing_approval_id IS NULL OR finalizing_approval_id = ?)""",
                (approval_id, job_id, approval_id),
            )
            return cur.rowcount == 1

    def list_unapplied_approvals(self) -> list[sqlite3.Row]:
        """Approvals that were recorded but never got their side effect
        applied — e.g. the process died between record_approval_if_new and
        mark_approval_applied. Replayed once at startup."""
        return self.query("SELECT * FROM approvals WHERE applied = 0 ORDER BY received_at ASC")

    # -- Admin console ------------------------------------------------------
    #
    # Read/write helpers for admin_api.py. Kept pure-SQL like the rest of
    # this file — file I/O for a job's original/xtc bytes (e.g. "purge")
    # is the caller's responsibility, same division as convert.py/
    # xtc_writer.py already owning file I/O outside db.py.

    def list_jobs_for_admin(self) -> list[sqlite3.Row]:
        return self.query(
            """SELECT j.*,
                      (SELECT COUNT(*) FROM job_deliveries d WHERE d.job_id = j.job_id) AS delivered_count,
                      (SELECT a.action FROM approvals a
                         WHERE a.job_id = j.job_id ORDER BY a.received_at DESC LIMIT 1) AS last_action
               FROM jobs j
               ORDER BY j.created_at DESC"""
        )

    def clear_deliveries_and_reset_status(self, job_id: str) -> None:
        """"Requeue": forgets every device's delivery record for this job
        and puts it back to 'pending', so it's redownloaded on the next
        sync. Does not touch the job's files or its approval history."""
        with self.transaction() as conn:
            conn.execute("DELETE FROM job_deliveries WHERE job_id = ?", (job_id,))
            conn.execute("UPDATE jobs SET status = 'pending' WHERE job_id = ?", (job_id,))

    def delete_job_row(self, job_id: str) -> None:
        """"Purge": removes the job and its delivery records. Approval
        history rows are left in place for audit (they reference job_id
        but there's no FK constraint, so this is safe). Caller must
        unlink the job's original/xtc files on disk first."""
        with self.transaction() as conn:
            conn.execute("DELETE FROM job_deliveries WHERE job_id = ?", (job_id,))
            conn.execute("DELETE FROM jobs WHERE job_id = ?", (job_id,))

    def list_recent_approvals(self, limit: int = 50) -> list[sqlite3.Row]:
        return self.query(
            """SELECT a.*, j.title AS job_title
               FROM approvals a
               LEFT JOIN jobs j ON j.job_id = a.job_id
               ORDER BY a.received_at DESC
               LIMIT ?""",
            (limit,),
        )

    def list_recent_approvals_for_device(self, device_id: str, limit: int = 10) -> list[sqlite3.Row]:
        """Same shape as list_recent_approvals, scoped to one device — backs
        the on-device web UI's "recent activity" view (station mode only,
        see admin_api.py's device-approvals route), so a device only ever
        sees its own history, not the whole household's. Uses the existing
        idx_approvals_device index."""
        return self.query(
            """SELECT a.*, j.title AS job_title
               FROM approvals a
               LEFT JOIN jobs j ON j.job_id = a.job_id
               WHERE a.device_id = ?
               ORDER BY a.received_at DESC
               LIMIT ?""",
            (device_id, limit),
        )

    # -- Devices (admin) ------------------------------------------------------

    def list_devices(self) -> list[sqlite3.Row]:
        return self.query("SELECT * FROM devices ORDER BY paired_at DESC")

    def delete_device(self, device_id: str) -> None:
        """"Revoke": the device's bearer token stops authenticating
        immediately (sync_api._authenticate looks up the row by
        device_id, so a missing row is an auth failure, same as an
        unrecognized device today)."""
        with self.transaction() as conn:
            conn.execute("DELETE FROM devices WHERE device_id = ?", (device_id,))

    def rotate_device_token(self, device_id: str, new_token_hash: str) -> None:
        with self.transaction() as conn:
            conn.execute("UPDATE devices SET token_hash = ? WHERE device_id = ?", (new_token_hash, device_id))

    # -- Calendar feeds (admin-managed, synced to every device) ---------------

    def list_calendar_feeds(self) -> list[sqlite3.Row]:
        return self.query("SELECT * FROM calendar_feeds ORDER BY position ASC, id ASC")

    def add_calendar_feed(self, url: str, label: str) -> int:
        with self.transaction() as conn:
            next_position = conn.execute("SELECT COALESCE(MAX(position), -1) + 1 FROM calendar_feeds").fetchone()[0]
            cur = conn.execute(
                "INSERT INTO calendar_feeds (url, label, position) VALUES (?, ?, ?)", (url, label, next_position)
            )
            return cur.lastrowid

    def delete_calendar_feed(self, feed_id: int) -> None:
        with self.transaction() as conn:
            conn.execute("DELETE FROM calendar_feeds WHERE id = ?", (feed_id,))

    # -- Wi-Fi networks (admin-managed, synced to every device) ---------------

    def list_wifi_networks(self) -> list[sqlite3.Row]:
        return self.query("SELECT * FROM wifi_networks ORDER BY position ASC, id ASC")

    def add_or_update_wifi_network(self, ssid: str, password: str) -> int:
        """Upsert by ssid -- editing a saved network's password from the
        admin console updates the same row rather than creating a
        duplicate, matching the UNIQUE(ssid) constraint and the firmware
        side's own addOrUpdate() semantics it's applied through."""
        with self.transaction() as conn:
            next_position = conn.execute("SELECT COALESCE(MAX(position), -1) + 1 FROM wifi_networks").fetchone()[0]
            cur = conn.execute(
                """INSERT INTO wifi_networks (ssid, password, position) VALUES (?, ?, ?)
                   ON CONFLICT(ssid) DO UPDATE SET password=excluded.password""",
                (ssid, password, next_position),
            )
            row = conn.execute("SELECT id FROM wifi_networks WHERE ssid = ?", (ssid,)).fetchone()
            return row["id"]

    def delete_wifi_network(self, network_id: int) -> None:
        with self.transaction() as conn:
            conn.execute("DELETE FROM wifi_networks WHERE id = ?", (network_id,))
