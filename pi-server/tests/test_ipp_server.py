import struct
import threading
import time
import urllib.request

import pytest

from tests.conftest import make_test_pdf
from xteink_print_server.config import Config
from xteink_print_server.db import Database
from xteink_print_server.ipp_server import (
    IPP_OP_GET_PRINTER_ATTRIBUTES,
    IPP_OP_PRINT_JOB,
    IppServer,
    _build_response,
    _operation_attributes,
    parse_ipp_request,
)


def _ipp_request(operation_id: int, request_id: int, extra_attrs: bytes = b"", document: bytes = b"") -> bytes:
    out = bytearray()
    out += bytes([1, 1])  # version
    out += struct.pack(">H", operation_id)
    out += struct.pack(">I", request_id)
    out += bytes([0x01])  # operation-attributes-tag
    out += _attr_str(0x47, "attributes-charset", "utf-8")
    out += _attr_str(0x48, "attributes-natural-language", "en")
    out += _attr_str(0x45, "printer-uri", "ipp://localhost/ipp/print")
    out += extra_attrs
    out += bytes([0x03])  # end-of-attributes
    out += document
    return bytes(out)


def _attr_str(tag: int, name: str, value: str) -> bytes:
    name_b = name.encode()
    value_b = value.encode()
    return bytes([tag]) + struct.pack(">H", len(name_b)) + name_b + struct.pack(">H", len(value_b)) + value_b


@pytest.fixture
def running_ipp_server(config: Config, db: Database):
    config.ipp_host = "127.0.0.1"
    config.ipp_port = 0  # ephemeral
    server = IppServer(config, db)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    time.sleep(0.05)
    yield f"http://127.0.0.1:{port}/ipp/print", db
    server.shutdown()
    thread.join(timeout=2)


def test_get_printer_attributes_returns_ok_and_printer_name(running_ipp_server):
    url, _db = running_ipp_server
    request = _ipp_request(IPP_OP_GET_PRINTER_ATTRIBUTES, 1)
    req = urllib.request.Request(url, data=request, headers={"Content-Type": "application/ipp"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        body = resp.read()
    status_code = struct.unpack(">H", body[2:4])[0]
    assert status_code == 0x0000
    assert b"Xteink X4" in body


def test_print_job_ingests_document_and_stores_original(running_ipp_server):
    url, db = running_ipp_server
    pdf_bytes = make_test_pdf(pages=2)
    extra = _attr_str(0x49, "document-format", "application/pdf") + _attr_str(0x42, "job-name", "My Print")
    request = _ipp_request(IPP_OP_PRINT_JOB, 2, extra_attrs=extra, document=pdf_bytes)
    req = urllib.request.Request(url, data=request, headers={"Content-Type": "application/ipp"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        body = resp.read()
    status_code = struct.unpack(">H", body[2:4])[0]
    assert status_code == 0x0000

    jobs = db.list_all_jobs()
    assert len(jobs) == 1
    assert jobs[0]["title"] == "My Print"
    assert jobs[0]["page_count"] == 2
    assert jobs[0]["original_mime"] == "application/pdf"

    from pathlib import Path

    assert Path(jobs[0]["original_path"]).read_bytes() == pdf_bytes


def test_print_job_rejects_empty_document(running_ipp_server):
    url, db = running_ipp_server
    request = _ipp_request(IPP_OP_PRINT_JOB, 3, document=b"")
    req = urllib.request.Request(url, data=request, headers={"Content-Type": "application/ipp"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        body = resp.read()
    status_code = struct.unpack(">H", body[2:4])[0]
    assert status_code == 0x0400  # client-error-bad-request
    assert db.list_all_jobs() == []


def test_parse_ipp_request_roundtrip():
    extra = _attr_str(0x49, "document-format", "application/pdf")
    raw = _ipp_request(IPP_OP_PRINT_JOB, 42, extra_attrs=extra, document=b"docbytes")
    meta, document = parse_ipp_request(raw)
    assert meta["operation_id"] == str(IPP_OP_PRINT_JOB)
    assert meta["request_id"] == "42"
    assert meta["document-format"] == "application/pdf"
    assert document == b"docbytes"
