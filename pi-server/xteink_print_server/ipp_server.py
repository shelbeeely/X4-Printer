"""Minimal IPP/1.1 receiver so "Xteink X4" shows up in the normal print
dialog on Windows, macOS, Linux, Android, and iOS and accepts driverless
print jobs — no PPD/driver install needed on the client.

The wire-format knowledge here (attribute tag bytes, operation IDs, chunked
HTTP body handling, the Get-Printer-Attributes response shape) is modeled
directly on paperlesspaper/paperlessprinter's hand-rolled
``BaseHTTPRequestHandler`` IPP server — see docs/architecture.md for exactly
what is reused vs. what differs. This implementation is narrower on purpose:
paperlessprinter renders the incoming document to PNGs for a client to poll;
this server instead (a) keeps the original document bytes untouched (needed
later to send the *original* to the physical printer) and (b) converts to
XTC (not PNG) for the X4's own on-device paging UI.

Supported operations: Print-Job, Validate-Job, Create-Job, Send-Document,
Get-Printer-Attributes, Get-Jobs, Get-Job-Attributes, Cancel-Job. That is
everything the built-in driverless/AirPrint/IPP-Everywhere client path in
every major OS actually uses for a simple "print one document" flow.
"""

from __future__ import annotations

import datetime as _dt
import logging
import struct
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import urlsplit

from .config import Config
from .convert import ConversionError, SUPPORTED_MIME_TYPES, convert_document_to_xtc, render_thumbnail_jpeg
from .db import Database
from .util import sha256_file

logger = logging.getLogger("xteink.ipp")

_PROCESS_STARTED = time.monotonic()

# --- IPP wire constants -----------------------------------------------------

IPP_OP_PRINT_JOB = 0x0002
IPP_OP_VALIDATE_JOB = 0x0004
IPP_OP_CREATE_JOB = 0x0005
IPP_OP_SEND_DOCUMENT = 0x0006
IPP_OP_CANCEL_JOB = 0x0008
IPP_OP_GET_JOB_ATTRIBUTES = 0x0009
IPP_OP_GET_JOBS = 0x000A
IPP_OP_GET_PRINTER_ATTRIBUTES = 0x000B

IPP_STATUS_OK = 0x0000
IPP_STATUS_CLIENT_ERROR_BAD_REQUEST = 0x0400
IPP_STATUS_CLIENT_ERROR_NOT_FOUND = 0x0406
IPP_STATUS_SERVER_ERROR_INTERNAL = 0x0500

TAG_OPERATION_ATTRIBUTES = 0x01
TAG_JOB_ATTRIBUTES = 0x02
TAG_END_OF_ATTRIBUTES = 0x03
TAG_PRINTER_ATTRIBUTES = 0x04

DELIMITER_TAGS = {0x01, 0x02, 0x03, 0x04, 0x05}

VT_INTEGER = 0x21
VT_BOOLEAN = 0x22
VT_ENUM = 0x23
VT_DATETIME = 0x31
VT_TEXT_WITHOUT_LANGUAGE = 0x41
VT_NAME_WITHOUT_LANGUAGE = 0x42
VT_KEYWORD = 0x44
VT_URI = 0x45
VT_CHARSET = 0x47
VT_NATURAL_LANGUAGE = 0x48
VT_MIME_MEDIA_TYPE = 0x49

JOB_STATE_PROCESSING = 5
JOB_STATE_COMPLETED = 9


def _attr(tag: int, name: str, value: bytes) -> bytes:
    name_b = name.encode("utf-8")
    return bytes([tag]) + struct.pack(">H", len(name_b)) + name_b + struct.pack(">H", len(value)) + value


def _attr_str(tag: int, name: str, value: str) -> bytes:
    return _attr(tag, name, value.encode("utf-8"))


def _attr_i32(tag: int, name: str, value: int) -> bytes:
    return _attr(tag, name, struct.pack(">i", int(value)))


def _attr_bool(name: str, value: bool) -> bytes:
    return _attr(VT_BOOLEAN, name, b"\x01" if value else b"\x00")


def _attr_str_set(tag: int, name: str, values: list[str]) -> bytes:
    out = bytearray()
    for i, v in enumerate(values):
        value_b = v.encode("utf-8")
        if i == 0:
            out += _attr(tag, name, value_b)
        else:
            out += bytes([tag]) + struct.pack(">H", 0) + struct.pack(">H", len(value_b)) + value_b
    return bytes(out)


def _attr_datetime(name: str, value: _dt.datetime) -> bytes:
    v = value.astimezone(_dt.timezone.utc)
    encoded = struct.pack(">HBBBBBBcBB", v.year, v.month, v.day, v.hour, v.minute, v.second, 0, b"+", 0, 0)
    return _attr(VT_DATETIME, name, encoded)


def _operation_attributes() -> bytes:
    out = bytearray([TAG_OPERATION_ATTRIBUTES])
    out += _attr_str(VT_CHARSET, "attributes-charset", "utf-8")
    out += _attr_str(VT_NATURAL_LANGUAGE, "attributes-natural-language", "en")
    return bytes(out)


def _build_response(status_code: int, request_id: int, body: bytes) -> bytes:
    out = bytearray()
    out += bytes([1, 1])  # IPP/1.1
    out += struct.pack(">H", status_code)
    out += struct.pack(">I", request_id)
    out += body
    out += bytes([TAG_END_OF_ATTRIBUTES])
    return bytes(out)


def parse_ipp_request(raw: bytes) -> tuple[dict[str, str], bytes]:
    """Extracts operation id/request id plus a handful of attributes we care
    about, and returns the document bytes that follow end-of-attributes."""
    if len(raw) < 8:
        raise ValueError("IPP request too short")

    operation_id = struct.unpack(">H", raw[2:4])[0]
    request_id = struct.unpack(">I", raw[4:8])[0]
    meta: dict[str, str] = {"operation_id": str(operation_id), "request_id": str(request_id)}

    pos = 8
    last_name: Optional[bytes] = None
    interesting = {
        "document-format",
        "document-name",
        "job-name",
        "job-id",
        "job-uri",
        "printer-uri",
        "requesting-user-name",
        "which-jobs",
        "compression",
    }

    while pos < len(raw):
        tag = raw[pos]
        pos += 1
        if tag in DELIMITER_TAGS:
            if tag == TAG_END_OF_ATTRIBUTES:
                break
            continue
        if pos + 2 > len(raw):
            raise ValueError("truncated IPP attribute (name length)")
        name_len = struct.unpack(">H", raw[pos : pos + 2])[0]
        pos += 2
        if name_len == 0:
            name = last_name or b""
        else:
            name = raw[pos : pos + name_len]
            pos += name_len
            last_name = name
        if pos + 2 > len(raw):
            raise ValueError("truncated IPP attribute (value length)")
        value_len = struct.unpack(">H", raw[pos : pos + 2])[0]
        pos += 2
        value = raw[pos : pos + value_len]
        pos += value_len

        name_str = name.decode("utf-8", errors="ignore")
        if name_str in interesting:
            decoded = value.decode("utf-8", errors="ignore")
            meta[name_str] = decoded

    document = raw[pos:]
    return meta, document


def _job_attributes(printer_uri: str, ipp_job_id: int, job_state: int, job_name: str) -> bytes:
    out = bytearray()
    out += _attr_i32(VT_INTEGER, "job-id", ipp_job_id)
    out += _attr_str(VT_URI, "job-uri", f"{printer_uri.rstrip('/')}/job/{ipp_job_id}")
    out += _attr_str(VT_URI, "job-printer-uri", printer_uri)
    out += _attr_str(VT_NAME_WITHOUT_LANGUAGE, "job-name", job_name or "print job")
    out += _attr_str(VT_NAME_WITHOUT_LANGUAGE, "job-originating-user-name", "anonymous")
    out += _attr_i32(VT_ENUM, "job-state", job_state)
    out += _attr_str(VT_KEYWORD, "job-state-reasons", "none")
    out += _attr_i32(VT_INTEGER, "job-printer-up-time", max(1, int(time.monotonic() - _PROCESS_STARTED)))
    return bytes(out)


class IppRequestHandler(BaseHTTPRequestHandler):
    server_version = "XteinkPrintServer/0.1"
    protocol_version = "HTTP/1.1"

    # Injected by IppServer below.
    config: Config
    db: Database

    def log_message(self, fmt: str, *args) -> None:  # noqa: A003 - BaseHTTPRequestHandler API
        logger.debug("%s - %s", self.address_string(), fmt % args)

    def _printer_uri(self) -> str:
        host = self.headers.get("Host", f"{self.config.ipp_host}:{self.config.ipp_port}")
        return f"ipp://{host}/ipp/print"

    def _read_body(self) -> bytes:
        length = self.headers.get("Content-Length")
        if length is not None:
            return self.rfile.read(int(length))
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                line = self.rfile.readline(65536).strip()
                if b";" in line:
                    line = line.split(b";", 1)[0]
                size = int(line or b"0", 16)
                if size == 0:
                    while True:
                        trailer = self.rfile.readline(65536)
                        if not trailer or trailer in (b"\r\n", b"\n"):
                            break
                    break
                body += self.rfile.read(size)
                self.rfile.readline(2)
            return bytes(body)
        return b""

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        parsed = urlsplit(self.path)
        if parsed.path not in ("/", "/ipp/print"):
            self.send_response(404)
            self.end_headers()
            return

        raw = self._read_body()
        try:
            meta, document = parse_ipp_request(raw)
        except ValueError as exc:
            logger.warning("malformed IPP request: %s", exc)
            self._send_ipp(_build_response(IPP_STATUS_CLIENT_ERROR_BAD_REQUEST, 1, _operation_attributes()))
            return

        operation_id = int(meta["operation_id"])
        request_id = int(meta["request_id"])
        handler = {
            IPP_OP_GET_PRINTER_ATTRIBUTES: self._handle_get_printer_attributes,
            IPP_OP_VALIDATE_JOB: self._handle_validate_job,
            IPP_OP_PRINT_JOB: self._handle_print_job,
            IPP_OP_CREATE_JOB: self._handle_create_job,
            IPP_OP_SEND_DOCUMENT: self._handle_send_document,
            IPP_OP_GET_JOBS: self._handle_get_jobs,
            IPP_OP_GET_JOB_ATTRIBUTES: self._handle_get_job_attributes,
            IPP_OP_CANCEL_JOB: self._handle_cancel_job,
        }.get(operation_id)

        if handler is None:
            logger.info("unsupported IPP operation 0x%04x", operation_id)
            self._send_ipp(_build_response(IPP_STATUS_OK, request_id, _operation_attributes()))
            return

        try:
            self._send_ipp(handler(request_id, meta, document))
        except Exception:  # noqa: BLE001 - must always answer with a valid IPP response
            logger.exception("IPP operation 0x%04x failed", operation_id)
            self._send_ipp(_build_response(IPP_STATUS_SERVER_ERROR_INTERNAL, request_id, _operation_attributes()))

    def _send_ipp(self, body: bytes) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # -- Operation handlers ------------------------------------------------

    def _handle_get_printer_attributes(self, request_id: int, meta: dict, document: bytes) -> bytes:
        printer_uri = self._printer_uri()
        info_uri = f"http://{self.headers.get('Host', '')}/"
        printer_uuid = uuid.uuid5(uuid.NAMESPACE_URL, printer_uri)
        now = _dt.datetime.now(_dt.timezone.utc)
        up_time = max(1, int(time.monotonic() - _PROCESS_STARTED))

        attrs = bytearray(_operation_attributes())
        attrs += bytes([TAG_PRINTER_ATTRIBUTES])
        attrs += _attr_str(VT_URI, "printer-uri-supported", printer_uri)
        attrs += _attr_str(VT_KEYWORD, "uri-authentication-supported", "none")
        attrs += _attr_str(VT_KEYWORD, "uri-security-supported", "none")
        attrs += _attr_str(VT_NAME_WITHOUT_LANGUAGE, "printer-name", self.config.printer_name)
        attrs += _attr_str(VT_TEXT_WITHOUT_LANGUAGE, "printer-info", "Xteink X4 print inbox (via Raspberry Pi)")
        attrs += _attr_str(VT_TEXT_WITHOUT_LANGUAGE, "printer-location", "Home")
        attrs += _attr_str(
            VT_TEXT_WITHOUT_LANGUAGE, "printer-make-and-model", "Xteink X4 Print Inbox Virtual Printer"
        )
        attrs += _attr_str(VT_URI, "printer-more-info", info_uri)
        attrs += _attr_str(VT_URI, "printer-uuid", f"urn:uuid:{printer_uuid}")
        attrs += _attr_str(
            VT_TEXT_WITHOUT_LANGUAGE,
            "printer-device-id",
            "MFG:xteink-print-inbox;MDL:X4 Print Inbox;CMD:PDF,JPEG,PNG;",
        )
        attrs += _attr_str_set(VT_KEYWORD, "ipp-versions-supported", ["1.1", "2.0"])
        ops = [
            IPP_OP_PRINT_JOB,
            IPP_OP_VALIDATE_JOB,
            IPP_OP_CREATE_JOB,
            IPP_OP_SEND_DOCUMENT,
            IPP_OP_GET_JOB_ATTRIBUTES,
            IPP_OP_GET_JOBS,
            IPP_OP_GET_PRINTER_ATTRIBUTES,
            IPP_OP_CANCEL_JOB,
        ]
        ops_bytes = bytearray()
        for i, op in enumerate(ops):
            if i == 0:
                ops_bytes += _attr_i32(VT_ENUM, "operations-supported", op)
            else:
                ops_bytes += bytes([VT_ENUM]) + struct.pack(">H", 0) + struct.pack(">H", 4) + struct.pack(">i", op)
        attrs += bytes(ops_bytes)
        attrs += _attr_str(VT_CHARSET, "charset-configured", "utf-8")
        attrs += _attr_str(VT_CHARSET, "charset-supported", "utf-8")
        attrs += _attr_str(VT_NATURAL_LANGUAGE, "natural-language-configured", "en")
        attrs += _attr_str(VT_NATURAL_LANGUAGE, "generated-natural-language-supported", "en")
        attrs += _attr_bool("printer-is-accepting-jobs", True)
        attrs += _attr_i32(VT_ENUM, "printer-state", 3)  # idle
        attrs += _attr_str(VT_KEYWORD, "printer-state-reasons", "none")
        attrs += _attr_i32(VT_INTEGER, "queued-job-count", 0)
        attrs += _attr_i32(VT_INTEGER, "printer-up-time", up_time)
        attrs += _attr_datetime("printer-config-change-date-time", now)
        attrs += _attr_str(VT_MIME_MEDIA_TYPE, "document-format-default", "application/pdf")
        attrs += _attr_str_set(VT_MIME_MEDIA_TYPE, "document-format-supported", sorted(SUPPORTED_MIME_TYPES))
        attrs += _attr_str(VT_KEYWORD, "compression-supported", "none")
        attrs += _attr_bool("job-ids-supported", True)
        attrs += _attr_str(VT_KEYWORD, "media-default", "na_letter_8.5x11in")
        attrs += _attr_str_set(VT_KEYWORD, "media-supported", ["na_letter_8.5x11in", "iso_a4_210x297mm"])
        attrs += _attr_i32(VT_INTEGER, "copies-default", 1)
        attrs += _attr_str(VT_KEYWORD, "sides-default", "one-sided")
        attrs += _attr_str(VT_KEYWORD, "print-color-mode-default", "monochrome")
        attrs += _attr_str_set(VT_KEYWORD, "print-color-mode-supported", ["monochrome", "auto"])
        attrs += _attr_i32(VT_INTEGER, "pages-per-minute", 1)
        return _build_response(IPP_STATUS_OK, request_id, bytes(attrs))

    def _handle_validate_job(self, request_id: int, meta: dict, document: bytes) -> bytes:
        return _build_response(IPP_STATUS_OK, request_id, _operation_attributes())

    def _resolve_mime(self, meta: dict, document: bytes) -> str:
        declared = meta.get("document-format", "").lower()
        if document.startswith(b"%PDF"):
            return "application/pdf"
        if document.startswith(b"\xff\xd8\xff"):
            return "image/jpeg"
        if document.startswith(b"\x89PNG\r\n\x1a\n"):
            return "image/png"
        if declared in SUPPORTED_MIME_TYPES:
            return declared
        return "application/octet-stream"

    def _ingest_document(self, meta: dict, document: bytes) -> Optional[str]:
        """Converts + persists a submitted document. Returns the new job_id,
        or None if the document was empty/unsupported (job rejected)."""
        if not document:
            logger.warning("received empty document body, rejecting job")
            return None

        mime = self._resolve_mime(meta, document)
        if mime not in SUPPORTED_MIME_TYPES:
            logger.warning("unsupported document-format %r, rejecting job", mime)
            return None

        title = meta.get("document-name") or meta.get("job-name") or "Untitled document"

        # Populated by on_first_page below, from the exact already-rendered
        # first page — no second PDF render. A thumbnail-generation failure
        # must never block job ingestion (the XTC conversion that actually
        # matters already succeeded by the time this could fail), so
        # failures here are logged and simply leave the holder empty rather
        # than propagating.
        thumbnail_bytes_holder: list[bytes] = []

        def _try_render_thumbnail(img):
            try:
                thumbnail_bytes_holder.append(render_thumbnail_jpeg(img))
            except Exception as exc:  # noqa: BLE001 - must never block job ingestion
                logger.warning("thumbnail generation failed for job %r: %s", title, exc)

        try:
            xtc_bytes, page_count = convert_document_to_xtc(
                document,
                mime,
                title=title,
                target_width=self.config.panel_width,
                target_height=self.config.panel_height,
                dpi=self.config.render_dpi,
                on_first_page=_try_render_thumbnail,
            )
        except ConversionError as exc:
            logger.error("conversion failed for job %r: %s", title, exc)
            return None

        job_id = uuid.uuid4().hex
        ext = {"application/pdf": "pdf", "image/jpeg": "jpg", "image/png": "png"}.get(mime, "bin")
        original_path = self.config.originals_dir / f"{job_id}.{ext}"
        original_path.write_bytes(document)
        xtc_path = self.config.xtc_dir / f"{job_id}.xtc"
        xtc_path.write_bytes(xtc_bytes)

        thumbnail_path = self.config.thumbnails_dir / f"{job_id}.jpg"
        if thumbnail_bytes_holder:
            thumbnail_path.write_bytes(thumbnail_bytes_holder[0])

        stored_id = self.db.insert_job(
            title=title,
            source="ipp",
            original_path=str(original_path),
            original_mime=mime,
            original_bytes=len(document),
            xtc_path=str(xtc_path),
            xtc_bytes=len(xtc_bytes),
            xtc_sha256=sha256_file(xtc_path),
            page_count=page_count,
            thumbnail_path=str(thumbnail_path) if thumbnail_bytes_holder else "",
        )
        logger.info("ingested job %s %r (%d pages, %d bytes original)", stored_id, title, page_count, len(document))
        return stored_id

    def _handle_print_job(self, request_id: int, meta: dict, document: bytes) -> bytes:
        job_id = self._ingest_document(meta, document)
        if job_id is None:
            return _build_response(IPP_STATUS_CLIENT_ERROR_BAD_REQUEST, request_id, _operation_attributes())

        row = self.db.get_job(job_id)
        attrs = bytearray(_operation_attributes())
        attrs += bytes([TAG_JOB_ATTRIBUTES])
        attrs += _job_attributes(self._printer_uri(), row["ipp_job_id"], JOB_STATE_COMPLETED, row["title"])
        return _build_response(IPP_STATUS_OK, request_id, bytes(attrs))

    def _handle_create_job(self, request_id: int, meta: dict, document: bytes) -> bytes:
        # Create-Job (no document yet) is answered with a placeholder job
        # that Send-Document fills in. We don't persist anything until the
        # document actually arrives, so stash the pending job-name on the
        # connection via a synthetic negative id keyed by request_id is
        # overkill for a prototype: most drivers that use Create-Job send
        # the document in the very next request on the same connection, so
        # a simple in-process pending-name table keyed by client address is
        # sufficient here.
        self.server.pending_job_names[self.client_address] = meta.get("job-name") or "Untitled document"  # type: ignore[attr-defined]
        attrs = bytearray(_operation_attributes())
        attrs += bytes([TAG_JOB_ATTRIBUTES])
        attrs += _attr_i32(VT_INTEGER, "job-id", 0)
        attrs += _attr_str(VT_URI, "job-uri", f"{self._printer_uri().rstrip('/')}/job/pending")
        attrs += _attr_i32(VT_ENUM, "job-state", 3)  # pending
        return _build_response(IPP_STATUS_OK, request_id, bytes(attrs))

    def _handle_send_document(self, request_id: int, meta: dict, document: bytes) -> bytes:
        pending_names = getattr(self.server, "pending_job_names", {})  # type: ignore[attr-defined]
        name = pending_names.pop(self.client_address, None)
        if name:
            meta = {**meta, "job-name": name}
        job_id = self._ingest_document(meta, document)
        if job_id is None:
            return _build_response(IPP_STATUS_CLIENT_ERROR_BAD_REQUEST, request_id, _operation_attributes())
        row = self.db.get_job(job_id)
        attrs = bytearray(_operation_attributes())
        attrs += bytes([TAG_JOB_ATTRIBUTES])
        attrs += _job_attributes(self._printer_uri(), row["ipp_job_id"], JOB_STATE_COMPLETED, row["title"])
        return _build_response(IPP_STATUS_OK, request_id, bytes(attrs))

    def _handle_get_jobs(self, request_id: int, meta: dict, document: bytes) -> bytes:
        attrs = bytearray(_operation_attributes())
        for row in self.db.list_all_jobs():
            attrs += bytes([TAG_JOB_ATTRIBUTES])
            state = JOB_STATE_COMPLETED
            attrs += _job_attributes(self._printer_uri(), row["rowid"], state, row["title"])
        return _build_response(IPP_STATUS_OK, request_id, bytes(attrs))

    def _handle_get_job_attributes(self, request_id: int, meta: dict, document: bytes) -> bytes:
        try:
            ipp_job_id = int(meta.get("job-id", "0"))
        except ValueError:
            ipp_job_id = 0
        row = self.db.get_job_by_ipp_id(ipp_job_id)
        if row is None:
            return _build_response(IPP_STATUS_CLIENT_ERROR_NOT_FOUND, request_id, _operation_attributes())
        attrs = bytearray(_operation_attributes())
        attrs += bytes([TAG_JOB_ATTRIBUTES])
        attrs += _job_attributes(self._printer_uri(), ipp_job_id, JOB_STATE_COMPLETED, row["title"])
        return _build_response(IPP_STATUS_OK, request_id, bytes(attrs))

    def _handle_cancel_job(self, request_id: int, meta: dict, document: bytes) -> bytes:
        try:
            ipp_job_id = int(meta.get("job-id", "0"))
        except ValueError:
            ipp_job_id = 0
        row = self.db.get_job_by_ipp_id(ipp_job_id)
        if row is None:
            return _build_response(IPP_STATUS_CLIENT_ERROR_NOT_FOUND, request_id, _operation_attributes())
        self.db.set_job_status(row["job_id"], "deleted")
        return _build_response(IPP_STATUS_OK, request_id, _operation_attributes())


class IppServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, config: Config, db: Database):
        handler = type("BoundIppRequestHandler", (IppRequestHandler,), {"config": config, "db": db})
        super().__init__((config.ipp_host, config.ipp_port), handler)
        self.pending_job_names: dict = {}


def run_ipp_server(config: Config, db: Database, ready_event: Optional[threading.Event] = None) -> None:
    server = IppServer(config, db)
    logger.info("IPP server listening on %s:%d as %r", config.ipp_host, config.ipp_port, config.printer_name)
    if ready_event is not None:
        ready_event.set()
    server.serve_forever()
