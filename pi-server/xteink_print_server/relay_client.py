"""Pi-side relay poller: the Pi's half of "remote approval away from home"
(docs/protocol.md §2). Runs as a background thread that only ever makes
*outbound* HTTPS requests to the relay — no inbound port is opened on the
home network for this feature, matching the task's "the home Pi makes
outbound connections" requirement.

Every approval pulled from the relay is applied through the exact same
printer_forward.apply_approval() idempotent path the direct sync API uses
(sync_api.py), so an approval that somehow arrives via both the relay and,
later, directly from the device on the home LAN is a guaranteed no-op the
second time.
"""

from __future__ import annotations

import json
import logging
import threading
import time
import urllib.error
import urllib.request

from .config import Config
from .db import Database
from .printer_forward import apply_approval

logger = logging.getLogger("xteink.relay_client")


class RelayClient:
    def __init__(self, config: Config, db: Database):
        self.config = config
        self.db = db
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def enabled(self) -> bool:
        return bool(self.config.relay_url and self.config.relay_account_id and self.config.relay_account_token)

    def start(self) -> None:
        if not self.enabled:
            logger.info("relay client disabled (XTEINK_RELAY_URL not configured)")
            return
        self._thread = threading.Thread(target=self._run, name="relay-client", daemon=True)
        self._thread.start()
        logger.info("relay client started, polling %s every %ds", self.config.relay_url, self.config.relay_poll_interval_seconds)

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self.poll_once()
            except Exception:  # noqa: BLE001 - keep the poll loop alive across transient errors
                logger.exception("relay poll failed")
            self._stop.wait(self.config.relay_poll_interval_seconds)

    def _request(self, method: str, path: str, body: dict | None = None) -> dict:
        url = f"{self.config.relay_url.rstrip('/')}/relay/v1/{path.lstrip('/')}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.config.relay_account_token}")
        req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def poll_once(self) -> int:
        """Fetches pending approvals for this account, applies each, and
        acks it back to the relay. Returns the number applied."""
        account = self.config.relay_account_id
        try:
            result = self._request("GET", f"accounts/{account}/approvals/pending")
        except urllib.error.URLError as exc:
            logger.debug("relay unreachable: %s", exc)
            return 0

        approvals = result.get("approvals", [])
        applied_count = 0
        for envelope in approvals:
            try:
                outcome = apply_approval(
                    self.db,
                    self.config,
                    approval_id=envelope["approval_id"],
                    device_id=envelope["device_id"],
                    job_id=envelope["job_id"],
                    action=envelope["action"],
                    created_at=int(envelope["created_at"]),
                    received_via="relay",
                )
            except Exception:  # noqa: BLE001
                logger.exception("failed to apply relayed approval %s", envelope.get("approval_id"))
                continue

            applied_count += 1
            try:
                self._request("POST", f"accounts/{account}/approvals/{outcome.approval_id}/ack")
            except urllib.error.URLError as exc:
                logger.warning("applied approval %s but failed to ack to relay: %s", outcome.approval_id, exc)

        if applied_count:
            logger.info("applied %d relayed approval(s)", applied_count)
        return applied_count
