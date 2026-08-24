"""Entrypoint for the relay. Run directly
(``python -m relay_server.server``) or via install/xteink-relay.service."""

from __future__ import annotations

import logging
import signal
import ssl
import threading
import time

from .app import RelayServer
from .config import load_config
from .db import RelayDatabase
from .util import configure_logging

logger = logging.getLogger("xteink.relay.server")

PRUNE_INTERVAL_SECONDS = 3600


def _prune_loop(db: RelayDatabase, retention_days: int, stop: threading.Event) -> None:
    while not stop.is_set():
        cutoff = int(time.time()) - retention_days * 86400
        pruned = db.prune_delivered_older_than(cutoff)
        if pruned:
            logger.info("pruned %d delivered approval envelope(s) older than %d days", pruned, retention_days)
        stop.wait(PRUNE_INTERVAL_SECONDS)


def main() -> None:
    config = load_config()
    configure_logging(config.log_level)
    logger.info("starting xteink relay, data dir %s", config.data_dir)

    db = RelayDatabase(config.db_path)
    server = RelayServer(config, db)

    if config.tls_cert and config.tls_key and config.tls_cert.exists() and config.tls_key.exists():
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=str(config.tls_cert), keyfile=str(config.tls_key))
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        logger.info("relay listening on https://%s:%d", config.host, config.port)
    else:
        logger.warning(
            "TLS not configured (XTEINK_RELAY_TLS_CERT/KEY) — relay running WITHOUT TLS. "
            "Only acceptable behind a TLS-terminating reverse proxy (nginx/caddy) on the same host, "
            "never exposed to the internet in plaintext."
        )

    stop_event = threading.Event()
    prune_thread = threading.Thread(target=_prune_loop, args=(db, config.retention_days, stop_event), daemon=True)
    prune_thread.start()

    def _handle_signal(signum, frame):  # noqa: ANN001, ARG001
        logger.info("received signal %d, shutting down", signum)
        stop_event.set()
        server.shutdown()

    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    server.serve_forever()
    logger.info("xteink relay stopped")


if __name__ == "__main__":
    main()
