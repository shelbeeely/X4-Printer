# Future Ideas

Unscoped ideas worth revisiting later. Not committed to, not designed yet —
just notes so they don't get lost.

## Phone printing straight to the X4, cutting the Pi out of the middle

Today, ingestion always goes phone/computer → Pi (`ipp_server.py`) → XTC
conversion → X4 downloads on its next wake. The idea: let a phone hand a
document straight to the X4 itself — no Pi in the ingestion path at all —
when they're on the same network (home LAN, or the X4's own hotspot mode
per `docs/architecture.md` "On-device Web UI (opt-in)"). The X4 queues it
locally same as any other inbox job, and the *printing* side (the part
that still genuinely needs a Pi + CUPS + a physical printer) only happens
later, once the X4 gets internet access again.

Rough shape, to flesh out later:
- The X4's Web UI already accepts phone-driven actions in hotspot mode
  (`ui/WebUiServer.h`/`.cpp`) — this would add a new "upload a document"
  path there rather than only approve/keep/delete on existing jobs.
- Someone still has to convert the upload to XTC for the e-ink screen.
  Either the X4 does a lightweight conversion itself on receipt, or it
  stores the original as-is and defers conversion until it can reach the
  Pi — needs picking.
- The Pi/CUPS is still required to *physically print* (the X4 has no
  printer driver of its own), so this doesn't remove the Pi entirely —
  just moves it out of the ingestion path and back to being purely the
  thing that eventually receives the original and calls `lp` once the X4
  is back online, likely via the same sync path jobs already use in
  reverse (X4 → Pi instead of Pi → X4), or through the relay
  (`docs/relay.md`) when the X4 never rejoins the home LAN directly.
- Raises the same questions "View full document" already had to answer
  for originals living on the Pi (auth, size/RAM limits) — except now the
  X4 is the one holding an arbitrary phone-uploaded file, which is a new
  trust boundary worth thinking through (`docs/security.md`).
