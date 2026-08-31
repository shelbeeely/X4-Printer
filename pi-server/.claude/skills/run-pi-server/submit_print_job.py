#!/usr/bin/env python3
"""Submits a real IPP Print-Job request against a running pi-server
instance -- the same request-building logic
tests/integration/test_end_to_end.py uses, extracted here so an agent (or
a human) can fire a one-off print job at a live server without writing an
IPP client from scratch.

Usage: submit_print_job.py [ipp_url] [job_name] [pages]
  ipp_url  default http://127.0.0.1:6310/ipp/print
  job_name default "Smoke Test Print"
  pages    default 2
"""
import struct
import sys
import urllib.request

import fitz  # PyMuPDF


def make_pdf(text: str, pages: int = 1) -> bytes:
    doc = fitz.open()
    for i in range(pages):
        page = doc.new_page(width=612, height=792)
        page.insert_text((72, 72), f"{text} (page {i + 1})", fontsize=20)
    data = doc.tobytes()
    doc.close()
    return data


def attr(tag: int, name: str, value: str) -> bytes:
    name_b, value_b = name.encode(), value.encode()
    return bytes([tag]) + struct.pack(">H", len(name_b)) + name_b + struct.pack(">H", len(value_b)) + value_b


def submit(ipp_url: str, document: bytes, job_name: str, mime: str = "application/pdf") -> int:
    out = bytearray()
    out += bytes([1, 1])
    out += struct.pack(">H", 0x0002)  # Print-Job
    out += struct.pack(">I", 1)
    out += bytes([0x01])
    out += attr(0x47, "attributes-charset", "utf-8")
    out += attr(0x48, "attributes-natural-language", "en")
    out += attr(0x45, "printer-uri", ipp_url)
    out += attr(0x49, "document-format", mime)
    out += attr(0x42, "job-name", job_name)
    out += bytes([0x03])
    out += document

    req = urllib.request.Request(ipp_url, data=bytes(out), headers={"Content-Type": "application/ipp"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        body = resp.read()
    return struct.unpack(">H", body[2:4])[0]


if __name__ == "__main__":
    ipp_url = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:6310/ipp/print"
    job_name = sys.argv[2] if len(sys.argv) > 2 else "Smoke Test Print"
    pages = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    status = submit(ipp_url, make_pdf(job_name, pages=pages), job_name)
    print(f"IPP status: 0x{status:04x} ({'ok' if status == 0 else 'FAILED'})")
    sys.exit(0 if status == 0 else 1)
