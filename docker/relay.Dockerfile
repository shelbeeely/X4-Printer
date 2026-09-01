# Containerized relay for local dev/testing (docker-compose.test.yml).
# Real deployments run this on whatever small VPS the user points their Pi
# at (see docs/relay.md, install/focusink-relay.service) -- this image exists
# for docker-compose's test stack, not as the recommended deploy path.
FROM python:3.11-slim-bookworm

WORKDIR /app
COPY relay/relay_server ./relay_server

ENV FOCUSINK_RELAY_DATA_DIR=/data
VOLUME ["/data"]

EXPOSE 8843

CMD ["python", "-m", "relay_server.server"]
