import io
import os
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from xteink_print_server.config import Config
from xteink_print_server.db import Database


class FakeLpBinary(str):
    """Behaves as the executable path (str) for subprocess.run, while also
    exposing .log_path so tests can assert on what was invoked."""

    log_path: Path


@pytest.fixture
def fake_lp_binary(tmp_path: Path) -> FakeLpBinary:
    """A stand-in for `lp` that never touches a real printer: it records
    every invocation to a log file and prints CUPS-style output so
    printer_forward.py's job-id regex has something to parse."""
    script = tmp_path / "fake_lp"
    log = tmp_path / "fake_lp_calls.log"
    script.write_text(
        "#!/bin/sh\n"
        f"echo \"$@\" >> {log}\n"
        "echo 'request id is xteink-print-inbox-7 (1 file(s))'\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IEXEC)
    wrapped = FakeLpBinary(str(script))
    wrapped.log_path = log
    return wrapped


@pytest.fixture
def config(tmp_path: Path, fake_lp_binary: Path) -> Config:
    cfg = Config(
        data_dir=tmp_path / "data",
        cups_queue="TestPrinter",
        lp_binary=str(fake_lp_binary),
        panel_width=800,
        panel_height=480,
        tls_cert=tmp_path / "data" / "tls" / "server.crt",
        tls_key=tmp_path / "data" / "tls" / "server.key",
    )
    cfg.ensure_dirs()
    return cfg


@pytest.fixture
def db(config: Config) -> Database:
    return Database(config.db_path)


def make_test_pdf(text: str = "Hello, X4!", pages: int = 1) -> bytes:
    import fitz

    doc = fitz.open()
    for i in range(pages):
        page = doc.new_page(width=612, height=792)  # US Letter
        page.insert_text((72, 72), f"{text} (page {i + 1})", fontsize=24)
    data = doc.tobytes()
    doc.close()
    return data


def make_test_png(size=(400, 300), color="white") -> bytes:
    from PIL import Image

    img = Image.new("RGB", size, color)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()
