#!/bin/sh
# Bootstraps a real, working CUPS install with one "PDF" virtual printer
# (cups-pdf backend) that other services on the compose network can submit
# to. Two-phase: bring cupsd up once to configure it via lpadmin/cupsctl
# (they talk to the running daemon over IPP on localhost, so it has to
# already be up), then hand off to a foreground `cupsd -f` as PID 1 so the
# container's lifecycle is tied to the daemon, not to this script.
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
# doesn't, so container startup never silently ships with no queue at all.
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

# cupsd (invoked above with no -f) daemonizes itself -- forks into the
# background and returns immediately -- so it was never this shell's job
# to `wait`/`kill %1` on; find it by name instead and stop it before
# re-exec'ing a foreground instance as PID 1.
pkill -x cupsd 2>/dev/null || true
for i in $(seq 1 10); do
  pgrep -x cupsd >/dev/null 2>&1 || break
  sleep 1
done

exec /usr/sbin/cupsd -f
