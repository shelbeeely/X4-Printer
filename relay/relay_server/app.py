"""Relay HTTP API — docs/protocol.md §2. Every route is scoped under
/relay/v1/accounts/{account_id}/... and requires ``Authorization: Bearer
<account_token>`` matching that account. The relay stores only IDs,
actions, and timestamps (see RelayDatabase) — it has no concept of a
document and no code path ever touches document bytes, which is what makes
"the cloud relay does not need the original printable document" true by
construction rather than by policy.
"""

from __future__ import annotations

import json
import logging
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import urlsplit

from .config import RelayConfig
from .db import RelayDatabase
from .util import constant_time_eq, hash_token

logger = logging.getLogger("xteink.relay")

MAX_BODY_BYTES = 8192
VALID_ACTIONS = {"print", "keep", "delete"}


class RelayRequestHandler(BaseHTTPRequestHandler):
    server_version = "XteinkRelay/0.1"
    protocol_version = "HTTP/1.1"

    config: RelayConfig
    db: RelayDatabase

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        logger.debug("%s - %s", self.address_string(), fmt % args)

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self, max_bytes: int = MAX_BODY_BYTES) -> Optional[dict]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > max_bytes:
            return None
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return None

    def _authenticate(self, account_id: str) -> bool:
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            self._send_json(401, {"error": "missing credentials"})
            return False
        token = auth[len("Bearer ") :].strip()
        account = self.db.get_account(account_id)
        if account is None or not constant_time_eq(account["token_hash"], hash_token(token)):
            self._send_json(401, {"error": "invalid credentials"})
            return False
        return True

    def _route(self):
        """Returns (account_id, resource_parts) or (None, None) with a 404
        already sent."""
        parts = [p for p in urlsplit(self.path).path.split("/") if p]
        if len(parts) < 4 or parts[0] != "relay" or parts[1] != "v1" or parts[2] != "accounts":
            self._send_json(404, {"error": "not found"})
            return None, None
        return parts[3], parts[4:]

    def do_GET(self) -> None:  # noqa: N802
        account_id, rest = self._route()
        if account_id is None:
            return
        if not self._authenticate(account_id):
            return

        if len(rest) == 2 and rest[0] == "approvals" and rest[1] == "pending":
            approvals = [
                {
                    "approval_id": row["approval_id"],
                    "device_id": row["device_id"],
                    "job_id": row["job_id"],
                    "action": row["action"],
                    "created_at": row["created_at"],
                }
                for row in self.db.list_pending_approvals(account_id)
            ]
            self._send_json(200, {"approvals": approvals})
        elif len(rest) == 2 and rest[0] == "approvals":
            approval = self.db.get_approval(rest[1])
            if approval is None or approval["account_id"] != account_id:
                self._send_json(404, {"error": "not found"})
                return
            status = "already_applied" if approval["delivered"] else "pending"
            self._send_json(200, {"approval_id": approval["approval_id"], "status": status})
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        account_id, rest = self._route()
        if account_id is None:
            return
        if not self._authenticate(account_id):
            return

        if len(rest) == 1 and rest[0] == "approvals":
            self._handle_submit_approval(account_id)
        elif len(rest) == 3 and rest[0] == "approvals" and rest[2] == "ack":
            self._handle_ack(account_id, rest[1])
        else:
            self._send_json(404, {"error": "not found"})

    def _handle_submit_approval(self, account_id: str) -> None:
        body = self._read_json_body()
        if body is None:
            self._send_json(400, {"error": "invalid body"})
            return
        required = {"approval_id", "device_id", "job_id", "action", "created_at"}
        if not required.issubset(body.keys()):
            self._send_json(400, {"error": "missing fields"})
            return
        if body["action"] not in VALID_ACTIONS:
            self._send_json(400, {"error": "invalid action"})
            return

        self.db.record_approval_if_new(
            approval_id=str(body["approval_id"]),
            account_id=account_id,
            device_id=str(body["device_id"]),
            job_id=str(body["job_id"]),
            action=str(body["action"]),
            created_at=int(body["created_at"]),
        )
        # Idempotent by construction: whether this call created the row or
        # it already existed, the envelope is (or already was) queued.
        self._send_json(200, {"approval_id": body["approval_id"], "status": "queued"})

    def _handle_ack(self, account_id: str, approval_id: str) -> None:
        approval = self.db.get_approval(approval_id)
        if approval is None or approval["account_id"] != account_id:
            self._send_json(404, {"error": "not found"})
            return
        self.db.mark_delivered(approval_id)
        self._send_json(200, {"status": "ok"})


class RelayServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, config: RelayConfig, db: RelayDatabase):
        handler = type("BoundRelayRequestHandler", (RelayRequestHandler,), {"config": config, "db": db})
        super().__init__((config.host, config.port), handler)
