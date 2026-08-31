"""Unit tests for config.py's admin-console runtime-override layer
(save_runtime_overrides / load_config's _apply_runtime_overrides).

test_admin_api.py already exercises this end-to-end through the settings
HTTP endpoint, but always against the *same* long-lived Config instance --
it never checks that a value survives what a real server restart does:
build a brand-new Config() and have load_config() re-read the settings
file from disk. That's the actual invariant the module docstring promises
("the mental model is 'the GUI is the live control panel'") and is what's
covered here instead.
"""

import json
import stat
from pathlib import Path

from xteink_print_server.config import Config, load_config, save_runtime_overrides


def _config(data_dir: Path) -> Config:
    # Config.tls_cert/tls_key default to a fixed system path
    # (/var/lib/xteink-print-server/tls/...), NOT derived from data_dir --
    # see pi-server/.claude/skills/run-pi-server/SKILL.md's TLS gotcha.
    # ensure_dirs() tries to mkdir that path's parent, which a real (non-root)
    # CI runner can't write to -- every test here must override it to stay
    # inside tmp_path, same as conftest.py's `config` fixture already does.
    return Config(
        data_dir=data_dir,
        tls_cert=data_dir / "tls" / "server.crt",
        tls_key=data_dir / "tls" / "server.key",
    )


def test_saved_overrides_survive_a_fresh_load(tmp_path):
    data_dir = tmp_path / "data"
    cfg = _config(data_dir)
    cfg.ensure_dirs()
    save_runtime_overrides(cfg, {"cups_queue": "LivingRoomPrinter", "retention_days": 5})

    # Simulate a server restart: a brand-new Config pointed at the same
    # data_dir, loaded the same way server.py does at startup.
    restarted = _config(data_dir)
    restarted.ensure_dirs()
    from xteink_print_server.config import _apply_runtime_overrides

    _apply_runtime_overrides(restarted)

    assert restarted.cups_queue == "LivingRoomPrinter"
    assert restarted.retention_days == 5


def test_save_runtime_overrides_rejects_unknown_field(tmp_path):
    cfg = _config(tmp_path / "data")
    cfg.ensure_dirs()
    try:
        save_runtime_overrides(cfg, {"admin_password": "nice-try"})
        assert False, "expected ValueError"
    except ValueError:
        pass
    # The rejected call must not have partially applied anything.
    assert cfg.admin_password == ""


def test_settings_file_is_written_owner_only(tmp_path):
    cfg = _config(tmp_path / "data")
    cfg.ensure_dirs()
    save_runtime_overrides(cfg, {"relay_account_token": "super-secret"})

    mode = stat.S_IMODE(cfg.admin_settings_path.stat().st_mode)
    assert mode == 0o600


def test_load_config_ignores_corrupt_settings_file(tmp_path, monkeypatch):
    data_dir = tmp_path / "data"
    monkeypatch.setenv("XTEINK_DATA_DIR", str(data_dir))
    monkeypatch.setenv("XTEINK_CUPS_QUEUE", "EnvDefaultPrinter")
    monkeypatch.setenv("XTEINK_TLS_CERT", str(data_dir / "tls" / "server.crt"))
    monkeypatch.setenv("XTEINK_TLS_KEY", str(data_dir / "tls" / "server.key"))
    cfg = Config()
    cfg.ensure_dirs()
    cfg.admin_settings_path.write_text("{not valid json")

    loaded = load_config()

    # A corrupt settings file must degrade to "use the env-var defaults",
    # never crash server startup.
    assert loaded.cups_queue == "EnvDefaultPrinter"


def test_save_runtime_overrides_merges_with_existing_file(tmp_path):
    cfg = _config(tmp_path / "data")
    cfg.ensure_dirs()
    save_runtime_overrides(cfg, {"cups_queue": "First"})
    save_runtime_overrides(cfg, {"retention_days": 9})

    on_disk = json.loads(cfg.admin_settings_path.read_text())
    assert on_disk["cups_queue"] == "First"
    assert on_disk["retention_days"] == 9
    assert cfg.cups_queue == "First"
    assert cfg.retention_days == 9
