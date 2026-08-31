# Containerized pi-server for local dev/testing (docker-compose.test.yml)
# and for exercising printer_forward.py's real `lp` shell-out against a
# real CUPS daemon (docker/cups/) instead of the fake `lp` the unit/
# integration test suites use everywhere else -- see docs/testing.md.
#
# This is NOT the deployment story: real installs are a bare Raspberry Pi
# running the systemd unit in install/ (see docs/setup-pi.md) -- a Pi Zero
# W's 512MB RAM is already shared with CUPS/avahi/the OS, so this project
# deliberately has no container runtime in its actual deployment path.
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

COPY pi-server/xteink_print_server ./xteink_print_server
COPY pi-server/tools ./tools

ENV XTEINK_DATA_DIR=/data
VOLUME ["/data"]

EXPOSE 6310 8443 8090

CMD ["python", "-m", "xteink_print_server.server"]
