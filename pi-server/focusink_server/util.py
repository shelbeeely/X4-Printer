"""Small shared helpers."""

from __future__ import annotations

import hashlib
import logging
import secrets
import ssl
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from http.server import HTTPServer

    from .config import Config

logger = logging.getLogger("focusink.util")


def sha256_file(path, chunk_size: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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


def serve_with_optional_tls(server: "HTTPServer", config: "Config", label: str, ready_event=None) -> None:
    """Wraps `server`'s socket in TLS if config.tls_cert/tls_key exist
    (falling back to plaintext with a warning otherwise, same as a real
    printer's driverless endpoint has no better option), signals
    `ready_event` once listening, then blocks in serve_forever(). Shared by
    sync_api.run_sync_api() and admin_api.run_admin_api() — both listeners
    reuse the same cert, so this was previously near-duplicated in each."""
    if config.tls_cert.exists() and config.tls_key.exists():
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=str(config.tls_cert), keyfile=str(config.tls_key))
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        host, port = server.server_address[:2]
        logger.info("%s listening on https://%s:%d", label, host, port)
    else:
        logger.warning(
            "TLS cert/key not found at %s / %s — %s running WITHOUT TLS. "
            "Run pi-server/tools/gen_selfsigned_cert.py before exposing this beyond localhost.",
            config.tls_cert,
            config.tls_key,
            label,
        )
    if ready_event is not None:
        ready_event.set()
    server.serve_forever()
