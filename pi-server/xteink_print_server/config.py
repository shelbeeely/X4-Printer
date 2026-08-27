"""Central configuration, loaded from environment variables with sane defaults.

Kept as plain env-var lookups (no config-file parser dependency) so the
systemd unit in install/ is the single source of truth for a deployment:
every setting is an ``Environment=`` line there, easy to audit and to
override per-host without touching code.

**Exception**: the small set of fields the admin web console
(``admin_api.py``) can edit live (``cups_queue``, ``retention_days``, and
the ``relay_*`` fields) are layered on top of env-var defaults from
``<data_dir>/admin_settings.json`` at load time, and that file's values
*win* over the environment — the mental model is "the GUI is the live
control panel, env vars/systemd are just the initial defaults." Every
other field (hosts, ports, TLS paths) stays env-var/systemd-only, since
changing those live would require rebinding a socket. See
``save_runtime_overrides`` below, called from ``admin_api.py``.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from dataclasses import dataclass, field
from pathlib import Path

logger = logging.getLogger("xteink.config")

# Guards read-modify-write of admin_settings.json: the admin API runs on a
# ThreadingHTTPServer, so two concurrent settings POSTs could otherwise
# each read the same on-disk dict before either writes, and the second
# writer's save would silently clobber the first's (same class of problem
# db.py's Database._write_lock exists to prevent for jobs.db).
_SETTINGS_LOCK = threading.Lock()


def _env_str(name: str, default: str) -> str:
    value = os.getenv(name)
    return default if value is None or value == "" else value


def _env_int(name: str, default: int) -> int:
    value = os.getenv(name)
    return default if value is None or value == "" else int(value)


def _env_bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None or value == "":
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


@dataclass
class Config:
    # Storage layout. Everything lives under data_dir so a whole deployment
    # is one directory to back up.
    data_dir: Path = field(default_factory=lambda: Path(_env_str("XTEINK_DATA_DIR", "/var/lib/xteink-print-server")))

    # IPP listener (what OS print dialogs talk to). Plain HTTP on the LAN,
    # like every other driverless/AirPrint printer — IPP's own security
    # model is "trust the LAN segment", matching real hardware printers.
    # NOT port 631: cupsd (which this same Pi also runs, to drive the real
    # physical printer via `lp`/printer_forward.py) already owns 631 on this
    # host. mDNS clients get the real port from the advertised service
    # record, not by assuming 631, so a different port is transparent to
    # every OS's driverless print path — see install/avahi/*.service.
    ipp_host: str = field(default_factory=lambda: _env_str("XTEINK_IPP_HOST", "0.0.0.0"))
    ipp_port: int = field(default_factory=lambda: _env_int("XTEINK_IPP_PORT", 6310))
    printer_name: str = field(default_factory=lambda: _env_str("XTEINK_PRINTER_NAME", "Xteink X4"))
    render_dpi: int = field(default_factory=lambda: _env_int("XTEINK_RENDER_DPI", 150))

    # X4 sync API (device <-> Pi). TLS + bearer token, see docs/protocol.md.
    sync_host: str = field(default_factory=lambda: _env_str("XTEINK_SYNC_HOST", "0.0.0.0"))
    sync_port: int = field(default_factory=lambda: _env_int("XTEINK_SYNC_PORT", 8443))
    tls_cert: Path = field(default_factory=lambda: Path(_env_str("XTEINK_TLS_CERT", "/var/lib/xteink-print-server/tls/server.crt")))
    tls_key: Path = field(default_factory=lambda: Path(_env_str("XTEINK_TLS_KEY", "/var/lib/xteink-print-server/tls/server.key")))

    # X4 panel geometry — what xtc_writer renders pages to. 800x480 (X4).
    panel_width: int = field(default_factory=lambda: _env_int("XTEINK_PANEL_WIDTH", 800))
    panel_height: int = field(default_factory=lambda: _env_int("XTEINK_PANEL_HEIGHT", 480))

    # Physical printer forwarding (CUPS queue name, configured once at
    # install time by whatever driver/IPP-Everywhere setup the user already
    # has for their real printer).
    cups_queue: str = field(default_factory=lambda: _env_str("XTEINK_CUPS_QUEUE", ""))
    lp_binary: str = field(default_factory=lambda: _env_str("XTEINK_LP_BINARY", "lp"))

    # Cloud relay (optional; empty relay_url disables the poller thread).
    relay_url: str = field(default_factory=lambda: _env_str("XTEINK_RELAY_URL", ""))
    relay_account_id: str = field(default_factory=lambda: _env_str("XTEINK_RELAY_ACCOUNT_ID", ""))
    relay_account_token: str = field(default_factory=lambda: _env_str("XTEINK_RELAY_ACCOUNT_TOKEN", ""))
    relay_poll_interval_seconds: int = field(default_factory=lambda: _env_int("XTEINK_RELAY_POLL_INTERVAL", 20))
    relay_allow_document_sync: bool = field(default_factory=lambda: _env_bool("XTEINK_RELAY_ALLOW_DOCUMENT_SYNC", False))

    # Retention: how long originals/xtc/job rows survive after being
    # archived or deleted, so the Pi's SD card doesn't grow unbounded.
    retention_days: int = field(default_factory=lambda: _env_int("XTEINK_RETENTION_DAYS", 30))

    # Admin web console (admin_api.py). Empty admin_password disables the
    # listener entirely (server.py never starts the thread) — no
    # accidental silent-open admin surface. TLS is reused from
    # tls_cert/tls_key above when present, same as the sync API.
    admin_host: str = field(default_factory=lambda: _env_str("XTEINK_ADMIN_HOST", "0.0.0.0"))
    admin_port: int = field(default_factory=lambda: _env_int("XTEINK_ADMIN_PORT", 8090))
    admin_password: str = field(default_factory=lambda: _env_str("XTEINK_ADMIN_PASSWORD", ""))

    log_level: str = field(default_factory=lambda: _env_str("XTEINK_LOG_LEVEL", "INFO"))

    @property
    def db_path(self) -> Path:
        return self.data_dir / "jobs.db"

    @property
    def originals_dir(self) -> Path:
        return self.data_dir / "originals"

    @property
    def xtc_dir(self) -> Path:
        return self.data_dir / "xtc"

    @property
    def thumbnails_dir(self) -> Path:
        return self.data_dir / "thumbnails"

    def ensure_dirs(self) -> None:
        for d in (self.data_dir, self.originals_dir, self.xtc_dir, self.thumbnails_dir, self.tls_cert.parent):
            d.mkdir(parents=True, exist_ok=True)

    @property
    def admin_settings_path(self) -> Path:
        return self.data_dir / "admin_settings.json"


# Fields the admin console is allowed to edit live (see module docstring).
# Everything else (hosts, ports, TLS paths, the admin password itself)
# stays env-var/systemd-only.
RUNTIME_OVERRIDABLE_FIELDS = {
    "cups_queue",
    "retention_days",
    "relay_url",
    "relay_account_id",
    "relay_account_token",
    "relay_poll_interval_seconds",
    "relay_allow_document_sync",
}


def _apply_runtime_overrides(cfg: Config) -> None:
    path = cfg.admin_settings_path
    if not path.exists():
        return
    try:
        overrides = json.loads(path.read_text())
    except (OSError, ValueError):
        logger.warning("could not read %s, ignoring saved admin settings", path)
        return
    for key, value in overrides.items():
        if key in RUNTIME_OVERRIDABLE_FIELDS:
            setattr(cfg, key, value)


def save_runtime_overrides(cfg: Config, overrides: dict) -> None:
    """Persists `overrides` to disk and applies them to `cfg` in place, so
    every component holding a reference to this same Config instance
    (sync_api, ipp_server, printer_forward, relay_client) sees the change
    immediately with no restart. Only RUNTIME_OVERRIDABLE_FIELDS may be
    set — the caller (admin_api.py) is expected to validate/coerce types
    before calling this.

    The file can hold relay_account_token in plaintext, so it's written
    0600 (owner read/write only), same intent as
    tools/gen_selfsigned_cert.py's handling of the TLS private key."""
    unknown = set(overrides) - RUNTIME_OVERRIDABLE_FIELDS
    if unknown:
        raise ValueError(f"not a live-editable setting: {sorted(unknown)}")

    path = cfg.admin_settings_path
    with _SETTINGS_LOCK:
        existing: dict = {}
        if path.exists():
            try:
                existing = json.loads(path.read_text())
            except (OSError, ValueError):
                existing = {}
        existing.update(overrides)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(existing, indent=2) + "\n")
        path.chmod(0o600)

    for key, value in overrides.items():
        setattr(cfg, key, value)


def load_config() -> Config:
    cfg = Config()
    cfg.ensure_dirs()
    _apply_runtime_overrides(cfg)
    return cfg
