#!/bin/sh
# Bootstraps a real, working CUPS install with one "PDF" virtual printer
# (cups-pdf backend) that other services on the compose network can submit
# to. Starts cupsd once, in the foreground, as a background job of this
# script (not a kill-and-respawn dance -- an earlier version of this file
# started a backgrounding `cupsd`, configured it via lpadmin/cupsctl, then
# killed and re-exec'd a *second*, foreground instance; that left a real
# window where the queue config lpadmin had just written could race the
# second daemon's own startup reading it back, and was the actual cause of
# CI seeing "the printer or class does not exist" moments after the
# healthcheck had reported healthy). One daemon, configured in place, no
# restart.
set -eu

/usr/sbin/cupsd -f &
CUPSD_PID=$!
trap 'kill "$CUPSD_PID" 2>/dev/null; wait "$CUPSD_PID" 2>/dev/null' TERM INT

for i in $(seq 1 30); do
  if lpstat -h localhost:631 -r >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

# Remote/anonymous access, no CUPS-level auth -- this container only ever
# exists on docker-compose's private test network (docker-compose.test.yml),
# never exposed beyond it. `cupsctl --remote-any` is the standard shortcut
# for "accept connections from other hosts, no auth" (opens Listen/Allow
# directives it would otherwise take several individual `cupsctl` calls to
# set). cupsctl applies to the already-running daemon above -- no restart.
cupsctl --remote-any WebInterface=no

# printer-driver-cups-pdf's own postinst normally registers a "PDF" queue
# automatically; this is a defensive fallback for images/versions where it
# doesn't (observed in CI: "CUPS failed to reload its configuration! /
# Skipped automated creation of the PDF queue." during `apt-get install`,
# because cupsd isn't running inside a `docker build` layer for the
# postinst's own lpadmin call to reach), so container startup never
# silently ships with no queue at all.
if ! lpstat -p PDF >/dev/null 2>&1; then
  ppd="$(find /usr/share/ppd -iname 'CUPS-PDF*.ppd' 2>/dev/null | head -n1 || true)"
  if [ -n "$ppd" ]; then
    lpadmin -p PDF -E -v cups-pdf:/ -P "$ppd"
  else
    lpadmin -p PDF -E -v cups-pdf:/ -m raw
  fi
fi

# Belt-and-braces: whichever path created it, make sure it's enabled and
# accepting jobs (a fresh dpkg install can leave a queue disabled/rejecting
# until explicitly told otherwise).
cupsaccept PDF 2>/dev/null || true
cupsenable PDF 2>/dev/null || true
lpadmin -p PDF -E

echo "entrypoint.sh: PDF queue ready:"
lpstat -p PDF

# Marker file for docker-compose.test.yml's healthcheck -- see its comment
# for why `lpstat -p PDF` alone isn't a reliable enough signal.
touch /tmp/cups-ready

wait "$CUPSD_PID"
