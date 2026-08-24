"""X4 <-> Pi sync API (docs/protocol.md §1). HTTPS, per-device bearer token.

This is the endpoint the firmware's SyncClient talks to directly when the
X4 is on the home LAN: list pending jobs, stream an XTC file (with Range
support for resumable/interrupted downloads), ack a verified download, and
submit approvals. Approval application always goes through
printer_forward.apply_approval() so the direct path and the relay-drained
path (relay_client.py) share one idempotent code path.
"""

from __future__ import annotations

import json
import logging
import ssl
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import parse_qs, urlsplit

from .config import Config
from .db import Database
from .printer_forward import apply_approval
from .util import constant_time_eq, hash_token

logger = logging.getLogger("xteink.sync_api")

MAX_APPROVAL_BODY_BYTES = 8192
DOWNLOAD_CHUNK_BYTES = 64 * 1024


class SyncApiHandler(BaseHTTPRequestHandler):
    server_version = "XteinkSyncAPI/0.1"
    protocol_version = "HTTP/1.1"

    config: Config
    db: Database

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        logger.debug("%s - %s", self.address_string(), fmt % args)

    # -- auth ---------------------------------------------------------------

    def _authenticate(self) -> Optional[str]:
        """Returns the authenticated device_id, or None (and has already
        written a 401 response) if auth fails."""
        auth = self.headers.get("Authorization", "")
        device_id = self.headers.get("X-Device-Id", "")
        if not auth.startswith("Bearer ") or not device_id:
            self._send_json(401, {"error": "missing credentials"})
            return None
        token = auth[len("Bearer ") :].strip()
        device = self.db.get_device(device_id)
        if device is None or not constant_time_eq(device["token_hash"], hash_token(token)):
            self._send_json(401, {"error": "invalid credentials"})
            return None
        self.db.touch_device(device_id)
        return device_id

    # -- response helpers -----------------------------------------------------

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self, max_bytes: int = MAX_APPROVAL_BODY_BYTES) -> Optional[dict]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > max_bytes:
            return None
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return None

    # -- routing --------------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        parts = [p for p in parsed.path.split("/") if p]
        # /api/v1/...
        if parts[:2] != ["api", "v1"]:
            self._send_json(404, {"error": "not found"})
            return
        rest = parts[2:]

        if len(rest) == 3 and rest[0] == "devices" and rest[2] == "jobs":
            self._handle_list_jobs(rest[1], parsed.query)
        elif len(rest) == 3 and rest[0] == "devices" and rest[2] == "status":
            self._handle_status(rest[1])
        elif len(rest) == 3 and rest[0] == "jobs" and rest[2] == "xtc":
            self._handle_download_xtc(rest[1])
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        parts = [p for p in parsed.path.split("/") if p]
        if parts[:2] != ["api", "v1"]:
            self._send_json(404, {"error": "not found"})
            return
        rest = parts[2:]

        if len(rest) == 3 and rest[0] == "jobs" and rest[2] == "ack":
            self._handle_ack(rest[1])
        elif len(rest) == 1 and rest[0] == "approvals":
            self._handle_approval()
        else:
            self._send_json(404, {"error": "not found"})

    # -- handlers ---------------------------------------------------------------

    def _handle_status(self, path_device_id: str) -> None:
        device_id = self._authenticate()
        if device_id is None:
            return
        self._send_json(200, {"server_time": int(time.time()), "printer_ready": bool(self.config.cups_queue)})

    def _handle_list_jobs(self, path_device_id: str, query: str) -> None:
        device_id = self._authenticate()
        if device_id is None:
            return
        if device_id != path_device_id:
            self._send_json(403, {"error": "device id mismatch"})
            return
        qs = parse_qs(query)
        status = (qs.get("status", ["pending"])[0] or "pending").strip()

        if status == "all":
            rows = self.db.list_all_jobs()
        else:
            rows = self.db.list_pending_jobs_for_device(device_id)

        jobs = [
            {
                "job_id": row["job_id"],
                "title": row["title"],
                "created_at": row["created_at"],
                "xtc_bytes": row["xtc_bytes"],
                "xtc_sha256": row["xtc_sha256"],
                "page_count": row["page_count"],
                "status": row["status"],
            }
            for row in rows
        ]
        self._send_json(200, {"jobs": jobs, "server_time": int(time.time())})

    def _handle_download_xtc(self, job_id: str) -> None:
        device_id = self._authenticate()
        if device_id is None:
            return
        row = self.db.get_job(job_id)
        if row is None:
            self._send_json(404, {"error": "job not found"})
            return

        from pathlib import Path

        xtc_path = Path(row["xtc_path"])
        if not xtc_path.exists():
            self._send_json(410, {"error": "xtc file no longer available"})
            return

        total_size = row["xtc_bytes"]
        range_header = self.headers.get("Range")
        start, end = 0, total_size - 1
        status = 200
        if range_header and range_header.startswith("bytes="):
            try:
                range_spec = range_header[len("bytes=") :].split(",")[0]
                start_s, _, end_s = range_spec.partition("-")
                start = int(start_s) if start_s else 0
                end = int(end_s) if end_s else total_size - 1
                end = min(end, total_size - 1)
                status = 206
            except ValueError:
                start, end, status = 0, total_size - 1, 200

        length = max(0, end - start + 1)
        self.send_response(status)
        self.send_header("Content-Type", "application/x-xtc")
        self.send_header("Content-Length", str(length))
        self.send_header("X-Content-SHA256", row["xtc_sha256"])
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{total_size}")
        self.end_headers()

        with open(xtc_path, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(DOWNLOAD_CHUNK_BYTES, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def _handle_ack(self, job_id: str) -> None:
        device_id = self._authenticate()
        if device_id is None:
            return
        body = self._read_json_body()
        if body is None:
            self._send_json(400, {"error": "invalid body"})
            return
        row = self.db.get_job(job_id)
        if row is None:
            self._send_json(404, {"error": "job not found"})
            return
        if body.get("sha256") != row["xtc_sha256"]:
            self._send_json(409, {"status": "hash_mismatch"})
            return
        self.db.mark_delivered(job_id, device_id)
        self._send_json(200, {"status": "ok"})

    def _handle_approval(self) -> None:
        device_id = self._authenticate()
        if device_id is None:
            return
        body = self._read_json_body()
        if body is None:
            self._send_json(400, {"error": "invalid body"})
            return
        required = {"approval_id", "device_id", "job_id", "action", "created_at"}
        if not required.issubset(body.keys()):
            self._send_json(400, {"error": "missing fields"})
            return
        if body["device_id"] != device_id:
            self._send_json(403, {"error": "device id mismatch"})
            return

        result = apply_approval(
            self.db,
            self.config,
            approval_id=str(body["approval_id"]),
            device_id=device_id,
            job_id=str(body["job_id"]),
            action=str(body["action"]),
            created_at=int(body["created_at"]),
            received_via="direct",
        )
        status_code = 200 if result.status != "rejected" else 400
        self._send_json(
            status_code,
            {
                "approval_id": result.approval_id,
                "status": result.status,
                "detail": result.detail,
                "cups_job_id": result.cups_job_id,
                "error": result.error,
            },
        )


class SyncApiServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, config: Config, db: Database):
        handler = type("BoundSyncApiHandler", (SyncApiHandler,), {"config": config, "db": db})
        super().__init__((config.sync_host, config.sync_port), handler)


def run_sync_api(config: Config, db: Database, ready_event=None) -> None:
    server = SyncApiServer(config, db)
    if config.tls_cert.exists() and config.tls_key.exists():
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=str(config.tls_cert), keyfile=str(config.tls_key))
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        logger.info("sync API listening on https://%s:%d", config.sync_host, config.sync_port)
    else:
        logger.warning(
            "TLS cert/key not found at %s / %s — sync API running WITHOUT TLS. "
            "Run pi-server/tools/gen_selfsigned_cert.py before exposing this beyond localhost.",
            config.tls_cert,
            config.tls_key,
        )
    if ready_event is not None:
        ready_event.set()
    server.serve_forever()
