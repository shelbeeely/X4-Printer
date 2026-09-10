from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


def _env_str(name: str, default: str) -> str:
    value = os.getenv(name)
    return default if value is None or value == "" else value


def _env_optional_path(name: str) -> Optional[Path]:
    # Deliberately None (not Path("")) when unset: Path("") normalizes to
    # Path(".") -- the current directory, which .exists() as True -- so a
    # naive `Path(_env_str(name, "")) or ...` check silently treats "TLS
    # not configured" as "TLS configured, cert/key both live in '.'" and
    # crashes deep in ssl.load_cert_chain with IsADirectoryError instead of
    # taking the plaintext fallback path server.py actually intends.
    value = os.getenv(name)
    return Path(value) if value else None


def _env_int(name: str, default: int) -> int:
    value = os.getenv(name)
    return default if value is None or value == "" else int(value)


@dataclass
class RelayConfig:
    data_dir: Path = field(default_factory=lambda: Path(_env_str("FOCUSINK_RELAY_DATA_DIR", "/var/lib/focusink-relay")))
    host: str = field(default_factory=lambda: _env_str("FOCUSINK_RELAY_HOST", "0.0.0.0"))
    port: int = field(default_factory=lambda: _env_int("FOCUSINK_RELAY_PORT", 8843))
    tls_cert: Optional[Path] = field(default_factory=lambda: _env_optional_path("FOCUSINK_RELAY_TLS_CERT"))
    tls_key: Optional[Path] = field(default_factory=lambda: _env_optional_path("FOCUSINK_RELAY_TLS_KEY"))
    # How long a delivered (acked) approval envelope is kept before pruning,
    # purely for operator debugging/audit — the relay never needs it again
    # once delivered=1.
    retention_days: int = field(default_factory=lambda: _env_int("FOCUSINK_RELAY_RETENTION_DAYS", 14))
    log_level: str = field(default_factory=lambda: _env_str("FOCUSINK_RELAY_LOG_LEVEL", "INFO"))

    @property
    def db_path(self) -> Path:
        return self.data_dir / "relay.db"

    def ensure_dirs(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)


def load_config() -> RelayConfig:
    cfg = RelayConfig()
    cfg.ensure_dirs()
    return cfg
