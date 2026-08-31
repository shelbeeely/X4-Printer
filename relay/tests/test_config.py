"""Unit tests for relay_server/config.py -- in particular
_env_optional_path()'s documented footgun (Path("") normalizing to Path(".")
and silently `.exists()`-ing as True), which had no test coverage despite
the module docstring-level comment explaining why it matters.
"""

from pathlib import Path

from relay_server.config import RelayConfig


def test_tls_paths_default_to_none_when_unset(monkeypatch, tmp_path):
    monkeypatch.delenv("XTEINK_RELAY_TLS_CERT", raising=False)
    monkeypatch.delenv("XTEINK_RELAY_TLS_KEY", raising=False)
    cfg = RelayConfig(data_dir=tmp_path)
    assert cfg.tls_cert is None
    assert cfg.tls_key is None


def test_tls_paths_empty_env_value_is_also_none(monkeypatch, tmp_path):
    # An explicitly-set-but-empty env var (e.g. `XTEINK_RELAY_TLS_CERT=`)
    # must take the same "not configured" path as an unset one -- not
    # Path("") -> Path(".") -> "exists, use the cwd as a cert file".
    monkeypatch.setenv("XTEINK_RELAY_TLS_CERT", "")
    cfg = RelayConfig(data_dir=tmp_path)
    assert cfg.tls_cert is None


def test_tls_paths_set_when_env_provided(monkeypatch, tmp_path):
    cert_path = str(tmp_path / "server.crt")
    monkeypatch.setenv("XTEINK_RELAY_TLS_CERT", cert_path)
    cfg = RelayConfig(data_dir=tmp_path)
    assert cfg.tls_cert == Path(cert_path)


def test_db_path_and_ensure_dirs(tmp_path):
    cfg = RelayConfig(data_dir=tmp_path / "nested" / "data")
    assert cfg.db_path == tmp_path / "nested" / "data" / "relay.db"
    assert not cfg.data_dir.exists()
    cfg.ensure_dirs()
    assert cfg.data_dir.is_dir()


def test_port_and_retention_defaults(monkeypatch, tmp_path):
    monkeypatch.delenv("XTEINK_RELAY_PORT", raising=False)
    monkeypatch.delenv("XTEINK_RELAY_RETENTION_DAYS", raising=False)
    cfg = RelayConfig(data_dir=tmp_path)
    assert cfg.port == 8843
    assert cfg.retention_days == 14
