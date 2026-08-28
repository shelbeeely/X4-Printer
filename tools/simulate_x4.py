#!/usr/bin/env python3
"""A fake X4 that speaks the exact sync protocol the firmware uses
(docs/protocol.md §1), for exercising the full Pi pipeline without
hardware — used by tests/integration/test_end_to_end.py and available as a
CLI for manual poking (see docs/setup-pi.md §7).

This intentionally mirrors the firmware's wake sequence
(docs/architecture.md "Deep sleep / wake sequence") step for step: list
pending jobs, download + verify SHA-256 + ack each one, then (optionally)
submit queued approvals — so it's a meaningful stand-in for firmware
behavior, not just an API smoke test.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import ssl
import sys
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class DownloadedJob:
    job_id: str
    title: str
    page_count: int
    xtc_bytes: bytes
    verified: bool
    # Optional landscape-strip variant (docs/protocol.md §1.1/§4) -- None
    # when this job has none.
    landscape_xtc_bytes: Optional[bytes] = None
    landscape_verified: bool = False


@dataclass
class X4Client:
    base_url: str  # e.g. https://pi.local:8443/api/v1
    device_id: str
    device_token: str
    ca_cert: Optional[Path] = None
    verify: bool = True
    timeout: float = 15.0
    _relay_base_url: Optional[str] = None
    _relay_account_id: Optional[str] = None
    _relay_account_token: Optional[str] = None

    def _ssl_context(self) -> Optional[ssl.SSLContext]:
        if not self.base_url.startswith("https://"):
            return None
        if not self.verify:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            return ctx
        ctx = ssl.create_default_context(cafile=str(self.ca_cert) if self.ca_cert else None)
        return ctx

    def _request(self, method: str, url: str, body: Optional[dict] = None, headers: Optional[dict] = None):
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.device_token}")
        req.add_header("X-Device-Id", self.device_id)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        return urllib.request.urlopen(req, timeout=self.timeout, context=self._ssl_context())

    @classmethod
    def from_pairing_file(cls, path: Path, ca_cert: Optional[Path] = None, verify: bool = True) -> "X4Client":
        pairing = json.loads(Path(path).read_text())
        client = cls(
            base_url=pairing["pi_base_url"],
            device_id=pairing["device_id"],
            device_token=pairing["device_token"],
            ca_cert=ca_cert,
            verify=verify,
        )
        client._relay_base_url = pairing.get("relay_base_url")
        client._relay_account_id = pairing.get("relay_account_id")
        client._relay_account_token = pairing.get("relay_account_token")
        return client

    def list_pending_jobs(self) -> list[dict]:
        with self._request("GET", f"{self.base_url}/devices/{self.device_id}/jobs?status=pending") as resp:
            return json.loads(resp.read())["jobs"]

    def download_job(self, job_id: str, expected_sha256: str, variant: Optional[str] = None) -> DownloadedJob:
        url = f"{self.base_url}/jobs/{job_id}/xtc"
        if variant is not None:
            url += f"?variant={variant}"
        with self._request("GET", url) as resp:
            data = resp.read()
            server_sha = resp.headers.get("X-Content-SHA256", "")

        computed = hashlib.sha256(data).hexdigest()
        verified = computed == expected_sha256 == server_sha
        return DownloadedJob(job_id=job_id, title="", page_count=0, xtc_bytes=data, verified=verified)

    def ack_job(self, job_id: str, sha256: str, landscape_sha256: Optional[str] = None) -> dict:
        body = {"sha256": sha256}
        if landscape_sha256 is not None:
            body["landscape_sha256"] = landscape_sha256
        with self._request("POST", f"{self.base_url}/jobs/{job_id}/ack", body=body) as resp:
            return json.loads(resp.read())

    def submit_approval(self, job_id: str, action: str, approval_id: Optional[str] = None) -> dict:
        approval_id = approval_id or uuid.uuid4().hex
        body = {
            "approval_id": approval_id,
            "device_id": self.device_id,
            "job_id": job_id,
            "action": action,
            "created_at": int(time.time()),
        }
        with self._request("POST", f"{self.base_url}/approvals", body=body) as resp:
            return json.loads(resp.read())

    def sync_pending_jobs(self, download_dir: Optional[Path] = None) -> list[DownloadedJob]:
        """Mirrors the firmware wake sequence steps 3-5 (docs/architecture.md):
        list, download+verify, ack. Raises on a hash mismatch rather than
        silently accepting a corrupt file, matching the firmware's behavior
        of leaving the job pending for retry."""
        downloaded = []
        for manifest in self.list_pending_jobs():
            job = self.download_job(manifest["job_id"], manifest["xtc_sha256"])
            job.title = manifest["title"]
            job.page_count = manifest["page_count"]
            if not job.verified:
                raise RuntimeError(f"hash mismatch downloading job {manifest['job_id']}")

            landscape_sha256 = manifest.get("landscape_xtc_sha256")
            if landscape_sha256:
                landscape_job = self.download_job(manifest["job_id"], landscape_sha256, variant="landscape")
                if not landscape_job.verified:
                    raise RuntimeError(f"hash mismatch downloading landscape variant of job {manifest['job_id']}")
                job.landscape_xtc_bytes = landscape_job.xtc_bytes
                job.landscape_verified = True

            self.ack_job(job.job_id, manifest["xtc_sha256"], landscape_sha256=landscape_sha256)
            if download_dir is not None:
                download_dir.mkdir(parents=True, exist_ok=True)
                (download_dir / f"{job.job_id}.xtc").write_bytes(job.xtc_bytes)
                if job.landscape_xtc_bytes is not None:
                    (download_dir / f"{job.job_id}_landscape.xtc").write_bytes(job.landscape_xtc_bytes)
            downloaded.append(job)
        return downloaded


def _cmd_list_jobs(client: X4Client, args: argparse.Namespace) -> None:
    for job in client.list_pending_jobs():
        print(f"{job['job_id']}  {job['title']!r:40s} pages={job['page_count']:<4} bytes={job['xtc_bytes']}")


def _cmd_download(client: X4Client, args: argparse.Namespace) -> None:
    manifests = {j["job_id"]: j for j in client.list_pending_jobs()}
    manifest = manifests.get(args.job_id)
    if manifest is None:
        print(f"job {args.job_id} not in pending list (already synced?)", file=sys.stderr)
        sys.exit(1)
    job = client.download_job(args.job_id, manifest["xtc_sha256"])
    print(f"downloaded {len(job.xtc_bytes)} bytes, verified={job.verified}")
    if args.out:
        Path(args.out).write_bytes(job.xtc_bytes)
        print(f"wrote {args.out}")
    if job.verified:
        print(client.ack_job(args.job_id, manifest["xtc_sha256"]))


def _cmd_approve(client: X4Client, args: argparse.Namespace) -> None:
    print(client.submit_approval(args.job_id, args.action))


def _cmd_sync(client: X4Client, args: argparse.Namespace) -> None:
    jobs = client.sync_pending_jobs(download_dir=Path(args.download_dir) if args.download_dir else None)
    for job in jobs:
        print(f"synced {job.job_id}  {job.title!r} ({job.page_count} pages)")
    if not jobs:
        print("no pending jobs")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pairing-file", type=Path, help="device.json from pair_device.py")
    parser.add_argument("--pi-base-url")
    parser.add_argument("--device-id")
    parser.add_argument("--device-token")
    parser.add_argument("--ca-cert", type=Path)
    parser.add_argument("--insecure", action="store_true", help="skip TLS verification (testing only)")

    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list-jobs")
    p_download = sub.add_parser("download")
    p_download.add_argument("job_id")
    p_download.add_argument("--out", type=Path)
    p_approve = sub.add_parser("approve")
    p_approve.add_argument("job_id")
    p_approve.add_argument("action", choices=["print", "keep", "delete"])
    p_sync = sub.add_parser("sync")
    p_sync.add_argument("--download-dir", type=Path)

    args = parser.parse_args()

    if args.pairing_file:
        client = X4Client.from_pairing_file(args.pairing_file, ca_cert=args.ca_cert, verify=not args.insecure)
    else:
        if not (args.pi_base_url and args.device_id and args.device_token):
            parser.error("either --pairing-file or --pi-base-url/--device-id/--device-token are required")
        client = X4Client(
            base_url=args.pi_base_url,
            device_id=args.device_id,
            device_token=args.device_token,
            ca_cert=args.ca_cert,
            verify=not args.insecure,
        )

    {
        "list-jobs": _cmd_list_jobs,
        "download": _cmd_download,
        "approve": _cmd_approve,
        "sync": _cmd_sync,
    }[args.command](client, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
