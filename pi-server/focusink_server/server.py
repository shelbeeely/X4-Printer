"""Entrypoint: wires up the DB, IPP listener, sync API, and relay poller.

Run directly (``python -m focusink_server.server``) or via the systemd
unit in install/focusink-server.service.
"""

from __future__ import annotations

import logging
import signal
import threading

from .admin_api import run_admin_api
from .config import Config, load_config
from .db import Database
from .ipp_server import run_ipp_server
from .printer_forward import replay_unapplied_approvals
from .relay_client import RelayClient
from .sync_api import run_sync_api

logger = logging.getLogger("focusink.server")


def main() -> None:
    config = load_config()
    from .util import configure_logging

    configure_logging(config.log_level)

    logger.info("starting focusink server, data dir %s", config.data_dir)
    db = Database(config.db_path)

    replayed = replay_unapplied_approvals(db, config)
    if replayed:
        logger.warning("replayed %d approval(s) left unapplied by a previous run", replayed)

    # Constructed before the listener threads (rather than after, as
    # before) so it can be handed to the admin console, which needs a
    # live reference to start/stop the poll thread when relay settings
    # change (admin_api.py's _handle_post_settings).
    relay = RelayClient(config, db)

    ipp_ready = threading.Event()
    sync_ready = threading.Event()
    admin_ready = threading.Event()

    ipp_thread = threading.Thread(target=run_ipp_server, args=(config, db, ipp_ready), name="ipp-server", daemon=True)
    sync_thread = threading.Thread(target=run_sync_api, args=(config, db, sync_ready), name="sync-api", daemon=True)
    ipp_thread.start()
    sync_thread.start()

    admin_thread = None
    if config.admin_password:
        admin_thread = threading.Thread(
            target=run_admin_api, args=(config, db, relay, admin_ready), name="admin-api", daemon=True
        )
        admin_thread.start()
    else:
        logger.info("admin console disabled (FOCUSINK_ADMIN_PASSWORD not configured)")
        admin_ready.set()

    relay.start()

    ipp_ready.wait(timeout=10)
    sync_ready.wait(timeout=10)
    admin_ready.wait(timeout=10)
    logger.info("focusink server ready")

    stop_event = threading.Event()

    def _handle_signal(signum, frame):  # noqa: ANN001, ARG001
        logger.info("received signal %d, shutting down", signum)
        stop_event.set()

    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    stop_event.wait()
    relay.stop()
    logger.info("focusink server stopped")


if __name__ == "__main__":
    main()
