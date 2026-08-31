#!/bin/sh
# Bootstraps a real, working CUPS install with one "PDF" virtual printer
# (cups-pdf backend) that other services on the compose network can submit
# to.
#
# History, because both wrong turns here are non-obvious and worth not
# re-discovering:
#   1. First version: started `cupsd` (self-daemonizing), configured it via
#      lpadmin/cupsctl, then killed it and exec'd a *second*, foreground
#      `cupsd -f` instance as PID 1. CI showed the actual race that design
#      invited: the healthcheck passed against the first (soon-to-be-killed)
#      daemon, and printer_forward.py's real `lp` call moments later hit
#      the fresh second daemon with "the printer or class does not exist".
#   2. Second version: tried to avoid the restart by running `cupsd -f &`
#      (foreground mode, backgrounded as a shell job) and configuring that
#      one directly. CI showed a different, well-known-once-you-hit-it
#      problem: `cupsd -f &` under dash's non-interactive job control
#      breaks the client tools' local-socket connection ("cupsctl: Unable
#      to connect to server: Bad file descriptor").
# This version: start `cupsd` the normal way (no -f; it self-daemonizes,
# and that daemonized process's fds are unaffected by dash job-control
# quirks), configure it, and then just wait on that SAME already-running
# daemon by PID -- no restart, no backgrounded foreground mode, no window
# where the queue config could race a second daemon reading it back.
set -eu

/usr/sbin/cupsd

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
# set).
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
# for why `lpstat -p PDF` alone isn't a reliable enough signal on its own.
touch /tmp/cups-ready

# cupsd already daemonized itself above (still the exact same process the
# lpadmin/cupsctl calls just configured -- never restarted) and is running
# independently of this script now. Find its real pid and just wait on it,
# so the container's lifecycle tracks the daemon without ever touching it
# again; forward TERM/INT for a clean `docker compose down`.
CUPSD_PID="$(cat /run/cups/cupsd.pid 2>/dev/null || true)"
trap 'kill "$CUPSD_PID" 2>/dev/null || true' TERM INT
if [ -n "$CUPSD_PID" ]; then
  while kill -0 "$CUPSD_PID" 2>/dev/null; do
    sleep 1
  done
else
  # Debian's cupsd is expected to write /run/cups/cupsd.pid; this is a
  # fallback in case that path ever differs, so the container still stays
  # up for as long as the daemon is actually answering requests.
  while lpstat -h localhost:631 -r >/dev/null 2>&1; do
    sleep 5
  done
fi
