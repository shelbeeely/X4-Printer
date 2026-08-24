import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from relay_server.config import RelayConfig
from relay_server.db import RelayDatabase


@pytest.fixture
def config(tmp_path: Path) -> RelayConfig:
    cfg = RelayConfig(data_dir=tmp_path / "data")
    cfg.ensure_dirs()
    return cfg


@pytest.fixture
def db(config: RelayConfig) -> RelayDatabase:
    return RelayDatabase(config.db_path)
