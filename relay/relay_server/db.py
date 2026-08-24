"""Relay storage. Deliberately the smallest possible schema: an account
(one per household/Pi) and approval envelopes. No document bytes, ever —
see docs/protocol.md §2 and docs/relay.md.
"""

from __future__ import annotations

import contextlib
import sqlite3
import threading
import time
from pathlib import Path
from typing import Iterator, Optional

SCHEMA = """
CREATE TABLE IF NOT EXISTS accounts (
    account_id TEXT PRIMARY KEY,
    token_hash TEXT NOT NULL,
    name TEXT NOT NULL,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS approvals (
    approval_id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL,
    device_id TEXT NOT NULL,
    job_id TEXT NOT NULL,
    action TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    received_at INTEGER NOT NULL,
    delivered INTEGER NOT NULL DEFAULT 0,
    delivered_at INTEGER
);

CREATE INDEX IF NOT EXISTS idx_approvals_account_pending
    ON approvals(account_id, delivered);
"""


class RelayDatabase:
    def __init__(self, path: Path):
        self.path = path
        self._local = threading.local()
        self._write_lock = threading.Lock()
        with self._connect() as conn:
            conn.executescript(SCHEMA)

    def _connect(self) -> sqlite3.Connection:
        if not hasattr(self._local, "conn"):
            conn = sqlite3.connect(str(self.path), timeout=30, check_same_thread=False)
            conn.row_factory = sqlite3.Row
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA busy_timeout=30000")
            self._local.conn = conn
        return self._local.conn

    @contextlib.contextmanager
    def transaction(self) -> Iterator[sqlite3.Connection]:
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
        return self._connect().execute(sql, params).fetchall()

    def query_one(self, sql: str, params: tuple = ()) -> Optional[sqlite3.Row]:
        return self._connect().execute(sql, params).fetchone()

    # -- Accounts -----------------------------------------------------------

    def create_account(self, account_id: str, token_hash: str, name: str) -> None:
        with self.transaction() as conn:
            conn.execute(
                "INSERT INTO accounts (account_id, token_hash, name, created_at) VALUES (?, ?, ?, ?)",
                (account_id, token_hash, name, int(time.time())),
            )

    def get_account(self, account_id: str) -> Optional[sqlite3.Row]:
        return self.query_one("SELECT * FROM accounts WHERE account_id = ?", (account_id,))

    # -- Approvals ------------------------------------------------------------

    def record_approval_if_new(
        self, *, approval_id: str, account_id: str, device_id: str, job_id: str, action: str, created_at: int
    ) -> bool:
        with self.transaction() as conn:
            cur = conn.execute(
                """INSERT OR IGNORE INTO approvals
                   (approval_id, account_id, device_id, job_id, action, created_at, received_at, delivered)
                   VALUES (?, ?, ?, ?, ?, ?, ?, 0)""",
                (approval_id, account_id, device_id, job_id, action, created_at, int(time.time())),
            )
            return cur.rowcount == 1

    def get_approval(self, approval_id: str) -> Optional[sqlite3.Row]:
        return self.query_one("SELECT * FROM approvals WHERE approval_id = ?", (approval_id,))

    def list_pending_approvals(self, account_id: str) -> list[sqlite3.Row]:
        return self.query(
            "SELECT * FROM approvals WHERE account_id = ? AND delivered = 0 ORDER BY received_at ASC",
            (account_id,),
        )

    def mark_delivered(self, approval_id: str) -> None:
        with self.transaction() as conn:
            conn.execute(
                "UPDATE approvals SET delivered = 1, delivered_at = ? WHERE approval_id = ?",
                (int(time.time()), approval_id),
            )

    def prune_delivered_older_than(self, cutoff_epoch: int) -> int:
        with self.transaction() as conn:
            cur = conn.execute(
                "DELETE FROM approvals WHERE delivered = 1 AND delivered_at < ?", (cutoff_epoch,)
            )
            return cur.rowcount
