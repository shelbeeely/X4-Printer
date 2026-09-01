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
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import parse_qs, urlsplit

from .config import Config
from .db import Database
from .printer_forward import apply_approval
from .util import constant_time_eq, hash_token, serve_with_optional_tls

logger = logging.getLogger("focusink.sync_api")

MAX_APPROVAL_BODY_BYTES = 8192
DOWNLOAD_CHUNK_BYTES = 64 * 1024


class SyncApiHandler(BaseHTTPRequestHandler):
    server_version = "FocusinkSyncAPI/0.1"
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
        elif len(rest) == 3 and rest[0] == "devices" and rest[2] == "config":
            self._handle_device_config(rest[1])
        elif len(rest) == 3 and rest[0] == "jobs" and rest[2] == "xtc":
            self._handle_download_xtc(rest[1], parsed.query)
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

    def _handle_device_config(self, path_device_id: str) -> None:
        """docs/protocol.md §1.6 -- household-wide calendar feeds and Wi-Fi
        networks, managed from the admin console (admin_api.py) and pulled
        by every paired device on each sync. See config/CalendarConfig.h
        and config/WifiStore.h on the firmware side for how these get
        applied to the device's own SD-backed stores (a wholesale replace
        for calendars, an addOrUpdate merge for Wi-Fi -- never a device
        lockout from a Pi-side list that's missing the network it's
        currently on)."""
        device_id = self._authenticate()
        if device_id is None:
            return
        if device_id != path_device_id:
            self._send_json(403, {"error": "device id mismatch"})
            return

        calendars = [{"url": row["url"], "label": row["label"]} for row in self.db.list_calendar_feeds()]
        wifi_networks = [{"ssid": row["ssid"], "password": row["password"]} for row in self.db.list_wifi_networks()]
        self._send_json(200, {"calendars": calendars, "wifi_networks": wifi_networks, "server_time": int(time.time())})

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

        jobs = []
        for row in rows:
            job = {
                "job_id": row["job_id"],
                "title": row["title"],
                "created_at": row["created_at"],
                "xtc_bytes": row["xtc_bytes"],
                "xtc_sha256": row["xtc_sha256"],
                "page_count": row["page_count"],
                "status": row["status"],
            }
            # Optional: present only when this job has a landscape-strip
            # variant (see xtc_writer.prepare_landscape_strip_images) --
            # absent, not a null/empty placeholder, for jobs converted
            # before this feature existed or whose landscape rendering was
            # skipped (ipp_server._ingest_document's best-effort fallback).
            if row["xtc_landscape_sha256"]:
                job["landscape_xtc_bytes"] = row["xtc_landscape_bytes"]
                job["landscape_xtc_sha256"] = row["xtc_landscape_sha256"]
                job["landscape_page_count"] = row["xtc_landscape_page_count"]
            jobs.append(job)
        self._send_json(200, {"jobs": jobs, "server_time": int(time.time())})

    def _handle_download_xtc(self, job_id: str, query: str = "") -> None:
        device_id = self._authenticate()
        if device_id is None:
            return
        row = self.db.get_job(job_id)
        if row is None:
            self._send_json(404, {"error": "job not found"})
            return

        variant = (parse_qs(query).get("variant", ["normal"])[0] or "normal").strip()
        if variant == "landscape":
            path_col, bytes_col, sha_col = "xtc_landscape_path", "xtc_landscape_bytes", "xtc_landscape_sha256"
        elif variant == "normal":
            path_col, bytes_col, sha_col = "xtc_path", "xtc_bytes", "xtc_sha256"
        else:
            self._send_json(400, {"error": f"unknown variant {variant!r}"})
            return

        if not row[path_col]:
            self._send_json(404, {"error": f"{variant} variant not available for this job"})
            return

        from pathlib import Path

        xtc_path = Path(row[path_col])
        if not xtc_path.exists():
            self._send_json(410, {"error": "xtc file no longer available"})
            return

        total_size = row[bytes_col]
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
        self.send_header("X-Content-SHA256", row[sha_col])
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
        # Optional: a device that also downloaded the landscape-strip
        # variant includes its hash so one ack still covers the whole job.
        # Only enforced when the job actually has a landscape variant --
        # absent from the body just means "didn't check", not a mismatch.
        landscape_sha256 = body.get("landscape_sha256")
        if row["xtc_landscape_sha256"] and landscape_sha256 is not None:
            if landscape_sha256 != row["xtc_landscape_sha256"]:
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
    serve_with_optional_tls(server, config, "sync API", ready_event)
