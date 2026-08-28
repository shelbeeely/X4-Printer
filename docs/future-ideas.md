# Future Ideas

Unscoped ideas worth revisiting later. Not committed to, not designed yet —
just notes so they don't get lost.

## Phone printing over hotspot / LAN, queued until internet returns

Let a phone print directly to the Pi (or to the X4 itself) when it's on
the same network as the printer — either the home LAN or a phone-hosted
hotspot the Pi has joined — even if neither device has internet access at
that moment. The job would queue locally and only get delivered/synced
(e.g. to the X4, or wherever it ultimately needs to go) once one side
regains internet connectivity.

Rough shape, to flesh out later:
- The Pi already exposes a normal IPP/mDNS printer on the LAN (see
  `docs/architecture.md`), so phone printing on the *same* network may
  already work via each OS's native print flow (AirPrint on iOS, the
  Android print service) — worth verifying before building anything new.
- The interesting gap is the "hotspot with no internet" case: Pi joins a
  phone's hotspot, phone prints to it, job sits in the Pi's existing
  durable job queue (`pi-server/`), and syncs onward once either device
  is back online — likely reusing the existing relay sync path
  (`docs/relay.md`) rather than inventing a new one.
- Needs a story for the Pi discovering/joining a hotspot automatically
  (or at least easily) instead of requiring manual Wi-Fi reconfiguration.
