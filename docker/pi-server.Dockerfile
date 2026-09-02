# Same image, two jobs:
#   1. The recommended production deployment (see docker-compose.yml at
#      the repo root and docs/setup-pi.md) -- replaces the Python venv +
#      systemd unit from install/focusink-server.service. CUPS and
#      avahi-daemon still run on the HOST, not in this container (see
#      install/docker-host-setup.sh's header comment for why) -- this
#      image is only ever "run the Python app" now, same as it always was.
#   2. Local dev/testing (docker-compose.test.yml) and exercising
#      printer_forward.py's real `lp` shell-out against a real CUPS daemon
#      (docker/cups/) instead of the fake `lp` the unit/integration test
#      suites use everywhere else -- see docs/testing.md.
#
# Note for a genuine Raspberry Pi Zero W: 512MB RAM is already shared with
# CUPS/avahi/the OS, and Docker itself (dockerd/containerd, image layers)
# is real overhead on top of that -- see docs/setup-pi.md's callout. If
# RAM is tight on your specific hardware, docs/setup-pi.md's "Manual
# install (no Docker)" section avoids it entirely; on a Pi 3/4/5 this is a
# non-issue.
FROM python:3.11-slim-bookworm

# cups-client provides `lp`/`lpstat`, the CUPS client CLI printer_forward.py
# shells out to (see docs/architecture.md "CUPS queue is fixed at install
# time" -- always `lp -d <configured-queue>`, matching the real Pi's own
# local `lp`, just pointed at a remote CUPS server via $CUPS_SERVER here).
RUN apt-get update && apt-get install -y --no-install-recommends \
        cups-client \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY pi-server/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt pytest

COPY pi-server/focusink_server ./focusink_server
COPY pi-server/tools ./tools
COPY docker/pi-server-entrypoint.sh ./entrypoint.sh
RUN chmod +x ./entrypoint.sh

ENV FOCUSINK_DATA_DIR=/data
VOLUME ["/data"]

EXPOSE 6310 8443 8090

ENTRYPOINT ["./entrypoint.sh"]
CMD ["python", "-m", "focusink_server.server"]
