#!/bin/sh
# Bootstraps a real, working CUPS install with one "PDF" virtual printer
# (cups-pdf backend) that other services on the compose network can submit
# to.
#
# History, because none of these wrong turns are obvious and worth not
# re-discovering:
#   1. First version: started `cupsd` (self-daemonizing), configured it via
#      lpadmin/cupsctl, then killed it and exec'd a *second*, foreground
#      `cupsd -f` instance as PID 1. CI showed the actual race that design
#      invited: the healthcheck passed against the first (soon-to-be-killed)
#      daemon, and printer_forward.py's real `lp` call moments later hit
#      the fresh second daemon with "the printer or class does not exist".
#   2. Second version: tried to avoid the restart by running `cupsd -f &`
#      (foreground mode, backgrounded as a shell job). CI showed
#      "cupsctl: Unable to connect to server: Bad file descriptor" and it
#      looked like a dash job-control quirk specific to `-f &`.
#   3. Third version: went back to plain self-daemonizing `cupsd` (no -f,
#      no restart at all). CI showed the SAME "Bad file descriptor" error,
#      this time from `lpadmin` -- which rules out (2)'s theory. The actual
#      pattern across all three attempts: `lpstat` (an unprivileged query)
#      has connected successfully every single time; `cupsctl`/`lpadmin`
#      (both need local-admin trust) are the only commands that ever fail
#      this way. That points at CUPS's local-domain-socket admin auth
#      (peer-credential trust over /run/cups/cups.sock), not at cupsd's
#      foreground/background mode, which was a red herring.
# This version tests that theory directly: force every admin-privileged
# client command over TCP to localhost:631 (`-h localhost:631`) instead of
# the default local domain socket, since that's a different connection and
# auth path in libcups. If this *still* fails, the diagnostics below
# (process list, /run/cups contents, un-suppressed stderr) are there so the
# next attempt has real evidence instead of another guess.
set -eu

echo "entrypoint.sh: starting as $(id)"

# Defensive: clear any state that might have been left behind by the
# `apt-get install printer-driver-cups-pdf` postinst's own attempted
# cupsd reload during the image build (CI showed "CUPS failed to reload
# its configuration!" at that step) -- cupsd recreates this directory
# fresh on its own, so removing it first costs nothing either way.
rm -rf /run/cups
mkdir -p /run/cups

/usr/sbin/cupsd

for i in $(seq 1 30); do
  if lpstat -h localhost:631 -r >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

echo "entrypoint.sh: cupsd is answering; /run/cups contents:"
ls -la /run/cups 2>&1 || true

# Remote/anonymous access, no CUPS-level auth -- this container only ever
# exists on docker-compose's private test network (docker-compose.test.yml),
# never exposed beyond it. `-h localhost:631` forces this over TCP rather
# than the local domain socket -- see header comment.
cupsctl -h localhost:631 --remote-any WebInterface=no

# printer-driver-cups-pdf's own postinst normally registers a "PDF" queue
# automatically; this is a defensive fallback for images/versions where it
# doesn't, so container startup never silently ships with no queue at all.
if ! lpstat -h localhost:631 -p PDF >/dev/null 2>&1; then
  ppd="$(find /usr/share/ppd -iname 'CUPS-PDF*.ppd' 2>/dev/null | head -n1 || true)"
  if [ -n "$ppd" ]; then
    lpadmin -h localhost:631 -p PDF -E -v cups-pdf:/ -P "$ppd"
  else
    lpadmin -h localhost:631 -p PDF -E -v cups-pdf:/ -m raw
  fi
fi

# Belt-and-braces: whichever path created it, make sure it's enabled and
# accepting jobs (a fresh dpkg install can leave a queue disabled/rejecting
# until explicitly told otherwise).
cupsaccept -h localhost:631 PDF 2>/dev/null || true
cupsenable -h localhost:631 PDF 2>/dev/null || true
lpadmin -h localhost:631 -p PDF -E

echo "entrypoint.sh: PDF queue ready:"
lpstat -h localhost:631 -p PDF

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
