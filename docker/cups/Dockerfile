# A real CUPS daemon with a virtual "PDF" printer (cups-pdf backend,
# writes finished jobs to disk instead of a physical device), for
# docker-compose.test.yml's real-CUPS integration test
# (tests/docker/test_cups_integration.py). Exists purely so
# printer_forward.py's `lp -d <queue> <file>` shell-out (docs/architecture.md
# "CUPS queue is fixed at install time") gets exercised against an actual
# CUPS install at least once, instead of only ever against the fake `lp`
# every unit/integration test elsewhere in this repo uses.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        cups \
        cups-client \
        printer-driver-cups-pdf \
        procps \
    && rm -rf /var/lib/apt/lists/*

COPY docker/cups/cups-pdf.conf /etc/cups/cups-pdf.conf
COPY docker/cups/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh \
    && mkdir -p /output \
    && chmod 777 /output

EXPOSE 631
VOLUME ["/output"]

# /tmp/cups-ready is only touched by entrypoint.sh once the PDF queue is
# fully configured against the one long-lived cupsd this container runs --
# checking `lpstat -p PDF` alone isn't enough of a signal on its own (see
# entrypoint.sh's header comment for the race that taught us that).
HEALTHCHECK --interval=2s --timeout=5s --retries=30 \
    CMD test -f /tmp/cups-ready && lpstat -h localhost:631 -p PDF

ENTRYPOINT ["/entrypoint.sh"]
