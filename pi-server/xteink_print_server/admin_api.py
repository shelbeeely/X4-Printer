"""Admin web console: a password-gated dashboard for managing this Pi's
print inbox (jobs, paired devices, approval history, live settings) —
served from the same process as the IPP/sync listeners, same
pure-stdlib http.server approach as sync_api.py.

**Disabled by default.** run_admin_api() (wired from server.py) only
starts if config.admin_password is set, same "empty disables" pattern
relay_client.RelayClient.enabled already uses — there is no
silent-open admin surface. Auth is a single shared HTTP Basic password
(username ignored — one shared secret, not real accounts, matching the
project's existing "one bearer token" simplicity); a browser's native
Basic-auth prompt handles credential caching, so the UI (admin_ui/) needs
no client-side auth code at all.

Job actions reuse printer_forward.apply_approval() for print/keep/delete
— a synthetic approval_id/device_id="admin-console" goes through the
exact same idempotent path direct and relay-drained approvals use (see
db.py's SCHEMA: approvals.device_id has no FK constraint, so this is
safe). "requeue" and "purge" have no direct-sync-API equivalent and are
admin-only, backed by the new db.py methods.

GET .../jobs/{id}/original streams the untouched original document
(never converted to XTC) — reusing this exact same admin password gate
lets a phone browsing the X4's on-device web UI in station mode fetch it
directly from the Pi, entirely bypassing the X4 (see
docs/architecture.md "On-device Web UI full-document preview").

See docs/security.md "Admin web console" for the trust-boundary writeup.
"""

from __future__ import annotations

import base64
import binascii
import json
import logging
import mimetypes
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional
from urllib.parse import urlsplit

from .config import Config, RUNTIME_OVERRIDABLE_FIELDS, save_runtime_overrides
from .db import Database
from .printer_forward import apply_approval
from .relay_client import RelayClient
from .util import constant_time_eq, hash_token, new_token, serve_with_optional_tls

logger = logging.getLogger("xteink.admin_api")

MAX_BODY_BYTES = 8192

STATIC_DIR = Path(__file__).resolve().parent / "admin_ui"
STATIC_FILES = {
    "": "index.html",
    "index.html": "index.html",
    "app.js": "app.js",
    "style.css": "style.css",
}

# Settings the console can GET/POST, and how to coerce a JSON value for
# each — a subset of RUNTIME_OVERRIDABLE_FIELDS (kept in config.py as the
# single source of truth for *which* fields are live-editable; this dict
# only adds the type each one coerces to).
SETTINGS_TYPES: dict[str, type] = {
    "cups_queue": str,
    "retention_days": int,
    "relay_url": str,
    "relay_account_id": str,
    "relay_account_token": str,
    "relay_poll_interval_seconds": int,
    "relay_allow_document_sync": bool,
}
assert set(SETTINGS_TYPES) == RUNTIME_OVERRIDABLE_FIELDS

JOB_APPROVAL_ACTIONS = {"print", "keep", "delete"}
JOB_ADMIN_ONLY_ACTIONS = {"requeue", "purge"}


class AdminApiHandler(BaseHTTPRequestHandler):
    server_version = "XteinkAdminAPI/0.1"
    protocol_version = "HTTP/1.1"

    config: Config
    db: Database
    relay: RelayClient

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003
        logger.debug("%s - %s", self.address_string(), fmt % args)

    # -- auth ---------------------------------------------------------------

    def _authenticate(self) -> bool:
        """Returns True if the request carries the correct shared admin
        password over HTTP Basic auth; otherwise writes a 401 (with
        WWW-Authenticate, so the browser's native prompt handles the rest)
        and returns False."""
        auth = self.headers.get("Authorization", "")
        password = None
        if auth.startswith("Basic "):
            try:
                decoded = base64.b64decode(auth[len("Basic ") :]).decode("utf-8")
                _, _, password = decoded.partition(":")
            except (ValueError, binascii.Error):
                password = None
        if password is None or not constant_time_eq(password, self.config.admin_password):
            body = json.dumps({"error": "unauthorized"}).encode("utf-8")
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="X4 Print Inbox admin"')
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return False
        return True

    # -- CORS -----------------------------------------------------------------

    def _send_cors_headers(self) -> None:
        """Reflects the request's Origin back verbatim (never `*`) with
        Allow-Credentials — required because the X4 page's own fetch() uses
        credentials: "include" so the browser attaches its cached Basic-auth
        credentials cross-origin, and `*` is invalid/ignored by browsers for
        credentialed requests per the CORS spec. Only ever called for
        GET/OPTIONS on the one route (devices/{id}/approvals) the X4 page
        fetches inline — see docs/security.md "Admin web console" and
        docs/architecture.md's note on this route for why this doesn't widen
        what the admin password already grants."""
        origin = self.headers.get("Origin")
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Access-Control-Allow-Credentials", "true")

    # -- response helpers -----------------------------------------------------

    def _send_json(self, status: int, payload: dict, *, cors: bool = False) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if cors:
            self._send_cors_headers()
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

    def _serve_static(self, parts: list[str]) -> None:
        rel = "/".join(parts)
        filename = STATIC_FILES.get(rel)
        if filename is None:
            self._send_json(404, {"error": "not found"})
            return
        try:
            data = (STATIC_DIR / filename).read_bytes()
        except OSError:
            self._send_json(404, {"error": "not found"})
            return
        content_type, _ = mimetypes.guess_type(filename)
        self.send_response(200)
        self.send_header("Content-Type", content_type or "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # -- routing --------------------------------------------------------------

    def do_GET(self) -> None:  # noqa: N802
        if not self._authenticate():
            return
        parsed = urlsplit(self.path)
        parts = [p for p in parsed.path.split("/") if p]
        if parts[:3] != ["api", "admin", "v1"]:
            self._serve_static(parts)
            return
        rest = parts[3:]

        if rest == ["status"]:
            self._handle_status()
        elif rest == ["jobs"]:
            self._handle_list_jobs()
        elif len(rest) == 3 and rest[0] == "jobs" and rest[2] == "original":
            self._handle_get_original(rest[1])
        elif len(rest) == 3 and rest[0] == "jobs" and rest[2] == "thumbnail":
            self._handle_get_thumbnail(rest[1])
        elif rest == ["devices"]:
            self._handle_list_devices()
        elif len(rest) == 3 and rest[0] == "devices" and rest[2] == "approvals":
            self._handle_list_device_approvals(rest[1])
        elif rest == ["approvals"]:
            self._handle_list_approvals()
        elif rest == ["settings"]:
            self._handle_get_settings()
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        if not self._authenticate():
            return
        parsed = urlsplit(self.path)
        parts = [p for p in parsed.path.split("/") if p]
        if parts[:3] != ["api", "admin", "v1"]:
            self._send_json(404, {"error": "not found"})
            return
        rest = parts[3:]

        if len(rest) == 3 and rest[0] == "jobs" and rest[2] == "action":
            self._handle_job_action(rest[1])
        elif len(rest) == 3 and rest[0] == "devices" and rest[2] == "revoke":
            self._handle_revoke_device(rest[1])
        elif len(rest) == 3 and rest[0] == "devices" and rest[2] == "rotate-token":
            self._handle_rotate_token(rest[1])
        elif rest == ["settings"]:
            self._handle_post_settings()
        else:
            self._send_json(404, {"error": "not found"})

    def do_OPTIONS(self) -> None:  # noqa: N802
        """CORS preflight for the one credentialed cross-origin route
        (devices/{id}/approvals) — no _authenticate() call here, deliberately:
        preflight OPTIONS requests never carry credentials (that's the
        browser's doing, not a bug to work around), so gating this on the
        admin password would just make every preflight fail and the real GET
        would never be attempted. Any other path 404s the same way unmatched
        do_GET/do_POST routes do."""
        parsed = urlsplit(self.path)
        parts = [p for p in parsed.path.split("/") if p]
        if parts[:3] != ["api", "admin", "v1"]:
            self._send_json(404, {"error": "not found"})
            return
        rest = parts[3:]

        if len(rest) == 3 and rest[0] == "devices" and rest[2] == "approvals":
            self.send_response(204)
            self._send_cors_headers()
            self.send_header("Access-Control-Allow-Methods", "GET")
            self.send_header("Access-Control-Allow-Headers", "Authorization")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self._send_json(404, {"error": "not found"})

    # -- handlers: dashboard --------------------------------------------------

    def _handle_status(self) -> None:
        pending_jobs = self.db.query_one("SELECT COUNT(*) AS c FROM jobs WHERE status = 'pending'")["c"]
        device_count = self.db.query_one("SELECT COUNT(*) AS c FROM devices")["c"]
        unapplied = self.db.query_one("SELECT COUNT(*) AS c FROM approvals WHERE applied = 0")["c"]
        self._send_json(
            200,
            {
                "server_time": int(time.time()),
                "printer_ready": bool(self.config.cups_queue),
                "relay_enabled": self.relay.enabled,
                "relay_running": self.relay.running,
                "jobs_pending": pending_jobs,
                "device_count": device_count,
                "unapplied_approvals": unapplied,
            },
        )

    # -- handlers: jobs ---------------------------------------------------------

    def _handle_list_jobs(self) -> None:
        jobs = [
            {
                "job_id": row["job_id"],
                "title": row["title"],
                "created_at": row["created_at"],
                "status": row["status"],
                "page_count": row["page_count"],
                "original_bytes": row["original_bytes"],
                "original_mime": row["original_mime"],
                "xtc_bytes": row["xtc_bytes"],
                "delivered_count": row["delivered_count"],
                "last_action": row["last_action"],
            }
            for row in self.db.list_jobs_for_admin()
        ]
        self._send_json(200, {"jobs": jobs})

    def _serve_job_file(
        self, job_id: str, path_field: str, default_content_type: str, *, mime_field: str = "", inline: bool = False
    ) -> None:
        """Shared by _handle_get_original/_handle_get_thumbnail below — both
        are "look up the job, read one of its path columns, stream the
        bytes" with only the content-type source and one header differing.
        job_id-not-found and path-field-empty (e.g. a job with no
        thumbnail) both 404 the same way as a missing file, since none are
        distinct error states worth a special client-side message."""
        job = self.db.get_job(job_id)
        if job is None:
            self._send_json(404, {"error": "job not found"})
            return
        path_str = job[path_field]
        if not path_str:
            self._send_json(404, {"error": f"{path_field} not set"})
            return
        try:
            data = Path(path_str).read_bytes()
        except OSError:
            self._send_json(404, {"error": f"{path_field} file not found"})
            return
        content_type = (job[mime_field] if mime_field else None) or default_content_type
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        if inline:
            self.send_header("Content-Disposition", "inline")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _handle_get_original(self, job_id: str) -> None:
        """Streams the untouched original document (the file printer_forward
        never sees — only convert.py's XTC output goes to the X4) so a phone
        browsing the X4's on-device web UI in station mode can preview the
        real document, not just its e-ink rendition. Gated by the same admin
        password as everything else here (see module docstring) — the phone
        fetches this URL directly, bypassing the X4 entirely, so nothing here
        is stored on the device (see docs/architecture.md "On-device Web UI
        full-document preview")."""
        self._serve_job_file(job_id, "original_path", "application/octet-stream", mime_field="original_mime", inline=True)

    def _handle_get_thumbnail(self, job_id: str) -> None:
        """Streams a small JPEG thumbnail (convert.py's render_thumbnail_jpeg,
        generated once at ingest from the job's own first XTC page — see
        ipp_server.py's _ingest_document) for the on-device web UI's job
        cards, station mode only. 404s for any job ingested before this
        feature existed (thumbnail_path == '', the migration default — see
        db.py's _ensure_column)."""
        self._serve_job_file(job_id, "thumbnail_path", "image/jpeg")

    def _handle_job_action(self, job_id: str) -> None:
        body = self._read_json_body()
        if body is None or "action" not in body:
            self._send_json(400, {"error": "invalid body"})
            return
        action = str(body["action"])

        job = self.db.get_job(job_id)
        if job is None:
            self._send_json(404, {"error": "job not found"})
            return

        if action in JOB_APPROVAL_ACTIONS:
            result = apply_approval(
                self.db,
                self.config,
                approval_id=uuid.uuid4().hex,
                device_id="admin-console",
                job_id=job_id,
                action=action,
                created_at=int(time.time()),
                received_via="admin",
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
        elif action == "requeue":
            self.db.clear_deliveries_and_reset_status(job_id)
            self._send_json(200, {"status": "requeued"})
        elif action == "purge":
            for field in ("original_path", "xtc_path", "thumbnail_path"):
                # thumbnail_path is the first nullable/optional path column
                # (''  for jobs with no thumbnail — see db.py's migration
                # default) — the unguarded unlink() below was safe for the
                # other two only because they're NOT NULL with no
                # empty-string producer anywhere. Path("").unlink() would
                # resolve to the current working directory; guard against it
                # explicitly rather than relying on unlink() to fail safely.
                path_str = job[field]
                if path_str:
                    Path(path_str).unlink(missing_ok=True)
            self.db.delete_job_row(job_id)
            self._send_json(200, {"status": "purged"})
        else:
            self._send_json(400, {"error": f"unknown action {action!r}"})

    # -- handlers: devices -----------------------------------------------------

    def _handle_list_devices(self) -> None:
        devices = [
            {
                "device_id": row["device_id"],
                "name": row["name"],
                "account_id": row["account_id"],
                "paired_at": row["paired_at"],
                "last_seen_at": row["last_seen_at"],
            }
            for row in self.db.list_devices()
        ]
        self._send_json(200, {"devices": devices})

    def _handle_revoke_device(self, device_id: str) -> None:
        if self.db.get_device(device_id) is None:
            self._send_json(404, {"error": "device not found"})
            return
        self.db.delete_device(device_id)
        self._send_json(200, {"status": "revoked"})

    def _handle_rotate_token(self, device_id: str) -> None:
        if self.db.get_device(device_id) is None:
            self._send_json(404, {"error": "device not found"})
            return
        token = new_token()
        self.db.rotate_device_token(device_id, hash_token(token))
        # Returned once, plaintext — same as pair_device.py's device.json.
        # The admin copies this into the device's SD card and it's gone.
        self._send_json(200, {"status": "rotated", "device_token": token})

    # -- handlers: approvals -----------------------------------------------------

    def _handle_list_approvals(self) -> None:
        approvals = [dict(row) for row in self.db.list_recent_approvals(50)]
        self._send_json(200, {"approvals": approvals})

    def _handle_list_device_approvals(self, device_id: str) -> None:
        """Backs the on-device web UI's "recent activity" link (station
        mode only) — the phone opens this URL directly, same non-proxied
        pattern as _handle_get_original/_handle_get_thumbnail above. No
        "device exists" check (unlike _handle_revoke_device): a revoked
        device's device_id still has valid historical approval rows
        (delete_device leaves approvals alone — no FK constraint), and
        showing history for a just-revoked device from this same admin
        console is reasonable, not an error case.

        CORS-enabled (reflected origin + credentials, see
        _send_cors_headers) — the only route on this handler that is, since
        it's the only one the X4 page's own inline fetch() calls
        cross-origin; see docs/security.md "Admin web console"."""
        approvals = [dict(row) for row in self.db.list_recent_approvals_for_device(device_id, limit=10)]
        self._send_json(200, {"approvals": approvals}, cors=True)

    # -- handlers: settings -----------------------------------------------------

    def _handle_get_settings(self) -> None:
        self._send_json(200, {name: getattr(self.config, name) for name in SETTINGS_TYPES})

    def _handle_post_settings(self) -> None:
        body = self._read_json_body(max_bytes=MAX_BODY_BYTES)
        if body is None:
            self._send_json(400, {"error": "invalid body"})
            return

        overrides: dict = {}
        for key, value in body.items():
            expected = SETTINGS_TYPES.get(key)
            if expected is None:
                self._send_json(400, {"error": f"not a live-editable setting: {key!r}"})
                return
            try:
                if expected is bool:
                    if not isinstance(value, bool):
                        raise ValueError("expected a boolean")
                    overrides[key] = value
                elif expected is int:
                    # bool is an int subclass in Python; reject it explicitly
                    # so {"retention_days": true} doesn't silently become 1.
                    if isinstance(value, bool) or not isinstance(value, (int, str)):
                        raise ValueError("expected an integer")
                    overrides[key] = int(value)
                else:
                    if not isinstance(value, str):
                        raise ValueError("expected a string")
                    overrides[key] = value
            except (TypeError, ValueError):
                self._send_json(400, {"error": f"invalid value for {key!r}"})
                return

        was_enabled = self.relay.enabled
        save_runtime_overrides(self.config, overrides)
        is_enabled = self.relay.enabled

        # relay.start()/.stop() are only called once today (server.py's
        # main()) — replicate that lifecycle transition here rather than
        # relying on the poll loop to notice, since a disabled relay never
        # gets a thread to notice anything with.
        if is_enabled and not was_enabled and not self.relay.running:
            self.relay.start()
            logger.info("relay enabled via admin console")
        elif not is_enabled and was_enabled and self.relay.running:
            self.relay.stop()
            logger.info("relay disabled via admin console")

        self._send_json(200, {name: getattr(self.config, name) for name in SETTINGS_TYPES})


class AdminApiServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, config: Config, db: Database, relay: RelayClient):
        handler = type(
            "BoundAdminApiHandler",
            (AdminApiHandler,),
            {"config": config, "db": db, "relay": relay},
        )
        super().__init__((config.admin_host, config.admin_port), handler)


def run_admin_api(config: Config, db: Database, relay: RelayClient, ready_event=None) -> None:
    server = AdminApiServer(config, db, relay)
    serve_with_optional_tls(server, config, "admin console", ready_event)
