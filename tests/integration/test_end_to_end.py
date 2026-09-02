"""End-to-end test of the critical workflow from docs/architecture.md:

    Normal Print Dialog -> Pi (IPP) -> retain original + generate XTC
        -> X4 wakes, downloads pending documents (simulate_x4.py)
        -> X4 offline, user approves -> approval saved locally
        -> X4 gets network again, approval syncs (direct, then via relay)
        -> Pi sends the ORIGINAL document to the physical printer (CUPS)

Runs entirely in-process against real (not mocked) IppServer, SyncApiServer,
RelayServer, and RelayClient instances, on ephemeral localhost ports, with a
fake `lp` binary standing in for CUPS (see conftest.py) so the test suite
never touches a real printer. Plain HTTP (no TLS) is used for the in-process
sync API here purely to avoid generating/trusting a throwaway cert in every
test run; sync_api.py's actual TLS wrapping (tested indirectly via
pi-server/tests) is unrelated to the request routing/idempotency logic this
test is verifying.
"""

import struct
import sys
import time
from pathlib import Path

import fitz

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from simulate_x4 import X4Client  # noqa: E402


def _make_pdf(text: str, pages: int = 1) -> bytes:
    doc = fitz.open()
    for i in range(pages):
        page = doc.new_page(width=612, height=792)
        page.insert_text((72, 72), f"{text} (page {i + 1})", fontsize=20)
    data = doc.tobytes()
    doc.close()
    return data


def _attr(tag: int, name: str, value: str) -> bytes:
    name_b, value_b = name.encode(), value.encode()
    return bytes([tag]) + struct.pack(">H", len(name_b)) + name_b + struct.pack(">H", len(value_b)) + value_b


def _submit_ipp_print_job(ipp_url: str, document: bytes, job_name: str, mime: str = "application/pdf") -> None:
    import urllib.request

    out = bytearray()
    out += bytes([1, 1])
    out += struct.pack(">H", 0x0002)  # Print-Job
    out += struct.pack(">I", 1)
    out += bytes([0x01])
    out += _attr(0x47, "attributes-charset", "utf-8")
    out += _attr(0x48, "attributes-natural-language", "en")
    out += _attr(0x45, "printer-uri", ipp_url)
    out += _attr(0x49, "document-format", mime)
    out += _attr(0x42, "job-name", job_name)
    out += bytes([0x03])
    out += document

    req = urllib.request.Request(ipp_url, data=bytes(out), headers={"Content-Type": "application/ipp"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        body = resp.read()
    status = struct.unpack(">H", body[2:4])[0]
    assert status == 0x0000, f"IPP Print-Job failed: status=0x{status:04x}"


def test_print_dialog_to_approved_physical_print(pi_stack):
    """The core loop, direct path (X4 on home LAN the whole time)."""
    # 1. "Normal print dialog" -> Pi (IPP), retains original + generates XTC.
    pdf_bytes = _make_pdf("Electric Bill", pages=2)
    _submit_ipp_print_job(pi_stack["ipp_url"], pdf_bytes, "Electric Bill")

    jobs_in_db = pi_stack["db"].list_all_jobs()
    assert len(jobs_in_db) == 1
    job_row = jobs_in_db[0]
    assert job_row["title"] == "Electric Bill"
    assert Path(job_row["original_path"]).read_bytes() == pdf_bytes  # original retained byte-for-byte
    assert Path(job_row["xtc_path"]).exists()  # XTC preview generated

    # 2. "X4 wakes ... downloads pending documents to SD" (simulated).
    client = X4Client(
        base_url=pi_stack["sync_base_url"],
        device_id=pi_stack["device_id"],
        device_token=pi_stack["device_token"],
    )
    pending = client.list_pending_jobs()
    assert len(pending) == 1
    assert pending[0]["job_id"] == job_row["job_id"]

    downloaded = client.sync_pending_jobs()
    assert len(downloaded) == 1
    assert downloaded[0].verified
    assert downloaded[0].xtc_bytes == Path(job_row["xtc_path"]).read_bytes()

    # Landscape-strip variant (docs/protocol.md §4): ipp_server.py generates
    # one alongside the normal rendering for every job, so a real end-to-end
    # sync should download+verify it too, not just the normal file.
    assert job_row["xtc_landscape_path"]
    assert downloaded[0].landscape_verified
    assert downloaded[0].landscape_xtc_bytes == Path(job_row["xtc_landscape_path"]).read_bytes()

    # Once acked, the job drops out of this device's pending list (simulates
    # "X4 goes offline" — no more sync traffic needed for it).
    assert client.list_pending_jobs() == []

    # 3. "user reads and approves a document" -> "approval is saved locally"
    #    is the on-device outbox (firmware/src/store/ApprovalOutbox.*, not
    #    exercised here since this test has no firmware) -> "X4 later gets
    #    internet access, approval syncs to home Pi directly".
    result = client.submit_approval(job_row["job_id"], "print")
    assert result["status"] == "applied"
    assert result["detail"] == "printed"

    # 4. "Pi sends original document to physical network printer."
    call_log = pi_stack["fake_lp_binary"].log_path.read_text().strip().splitlines()
    assert len(call_log) == 1
    assert job_row["original_path"] in call_log[0]

    # 5. Idempotency: a retried approval (same approval_id, e.g. the device
    #    rebooted before it got the HTTP response and retries the outbox
    #    entry) must not print a second copy.
    retry_result = client.submit_approval(job_row["job_id"], "print", approval_id=result["approval_id"])
    assert retry_result["status"] == "already_applied"
    call_log_after_retry = pi_stack["fake_lp_binary"].log_path.read_text().strip().splitlines()
    assert len(call_log_after_retry) == 1  # still exactly one physical print


def test_remote_approval_via_relay_reaches_printer_exactly_once(pi_stack, relay_stack):
    """The away-from-home path: X4 -> relay -> Pi (polling) -> CUPS."""
    import urllib.request
    import json
    import uuid

    pdf_bytes = _make_pdf("Boarding Pass", pages=1)
    _submit_ipp_print_job(pi_stack["ipp_url"], pdf_bytes, "Boarding Pass")
    job_row = pi_stack["db"].list_all_jobs()[0]

    # X4 is "away from home": it can reach the relay but (in this test) we
    # simply never call the Pi's sync API directly for the approval — only
    # the relay, exactly as firmware would when the LAN sync endpoint isn't
    # reachable (see docs/architecture.md wake step 6, relay fallback).
    approval_id = uuid.uuid4().hex
    body = {
        "approval_id": approval_id,
        "device_id": pi_stack["device_id"],
        "job_id": job_row["job_id"],
        "action": "print",
        "created_at": int(time.time()),
    }
    req = urllib.request.Request(
        f"{relay_stack['base_url']}/accounts/{relay_stack['account_id']}/approvals",
        data=json.dumps(body).encode(),
        method="POST",
        headers={"Authorization": f"Bearer {relay_stack['account_token']}", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        assert json.loads(resp.read())["status"] == "queued"

    # Nothing has reached CUPS yet — the relay never touches the printer.
    assert not pi_stack["fake_lp_binary"].log_path.exists()

    # The Pi's relay poller (outbound-only, docs/protocol.md §2.2) picks it
    # up and applies it through the exact same idempotent code path as the
    # direct sync API.
    from focusink_server.relay_client import RelayClient

    pi_stack["config"].relay_url = relay_stack["base_url"].rsplit("/relay/v1", 1)[0]
    pi_stack["config"].relay_account_id = relay_stack["account_id"]
    pi_stack["config"].relay_account_token = relay_stack["account_token"]
    relay_client = RelayClient(pi_stack["config"], pi_stack["db"])
    applied = relay_client.poll_once()
    assert applied == 1

    call_log = pi_stack["fake_lp_binary"].log_path.read_text().strip().splitlines()
    assert len(call_log) == 1
    assert job_row["original_path"] in call_log[0]

    # The relay marks it delivered so a second poll (e.g. the next 20s
    # tick) does not re-apply it.
    applied_again = relay_client.poll_once()
    assert applied_again == 0
    call_log_after = pi_stack["fake_lp_binary"].log_path.read_text().strip().splitlines()
    assert len(call_log_after) == 1  # still exactly one physical print


def _make_png(color: str = "white", size: tuple[int, int] = (400, 300)) -> bytes:
    import io

    from PIL import Image

    img = Image.new("RGB", size, color)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def test_direct_upload_from_x4_to_printed_job(pi_stack):
    """The "direct upload" flow (docs/architecture.md "Direct upload",
    docs/protocol.md §1.7): a phone photo becomes a real job entirely
    on-device with no Pi involved at that moment -- the client-side
    WASM-encode step (tools/xtc-wasm/xtc_encoder.cpp) and browser UI
    aren't exercised here, only the sync-side upload this test *can*
    reach through simulate_x4.py's upload_original(), which mirrors
    SyncManager::uploadPendingOriginals(). The Pi ingests it under the
    X4's own job_id, so it lands under the exact id the device already has
    a Print approval queued against locally (its ApprovalOutbox, also not
    simulated here) -- proving the *existing* approval-apply path handles
    an x4_upload-sourced job identically to an IPP-sourced one, with no
    new print-path code."""
    client = X4Client(
        base_url=pi_stack["sync_base_url"],
        device_id=pi_stack["device_id"],
        device_token=pi_stack["device_token"],
    )

    import uuid

    job_id = uuid.uuid4().hex  # the X4 generates this itself, the moment the user picks a photo
    png_bytes = _make_png()

    assert pi_stack["db"].get_job(job_id) is None  # nothing on the Pi yet -- created purely on-device

    result = client.upload_original(job_id, png_bytes, "image/png", "Whiteboard Photo")
    assert result["job_id"] == job_id
    assert result["status"] == "created"

    job_row = pi_stack["db"].get_job(job_id)
    assert job_row is not None
    assert job_row["title"] == "Whiteboard Photo"
    assert job_row["source"] == "x4_upload"
    assert job_row["status"] == "pending"

    # A retried upload (the same connection-dropped-after-ingestion case
    # uploadPendingOriginals() would retry on the next wake) is a no-op,
    # not a duplicate job.
    retry_result = client.upload_original(job_id, png_bytes, "image/png", "Whiteboard Photo")
    assert retry_result["status"] == "already_exists"
    assert len(pi_stack["db"].list_all_jobs()) == 1

    # Once the original has landed, syncing a Print approval for this same
    # job_id goes through the exact same code path any Pi-originated job's
    # approval already does -- the key design property of this feature.
    approval = client.submit_approval(job_id, "print")
    assert approval["status"] == "applied"
    assert approval["detail"] == "printed"

    call_log = pi_stack["fake_lp_binary"].log_path.read_text().strip().splitlines()
    assert len(call_log) == 1
    assert job_row["original_path"] in call_log[0]
