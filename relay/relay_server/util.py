"""Small shared helpers (deliberately duplicated from pi-server/xteink_print_server/util.py
rather than shared as a common package — the relay and the Pi server are
separate deployables, often on separate machines/hosts, and this project
keeps that boundary real rather than introducing a shared-lib dependency
for a handful of one-line functions)."""

from __future__ import annotations

import hashlib
import logging
import secrets


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def new_token(nbytes: int = 32) -> str:
    return secrets.token_hex(nbytes)


def constant_time_eq(a: str, b: str) -> bool:
    return secrets.compare_digest(a, b)


def configure_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
