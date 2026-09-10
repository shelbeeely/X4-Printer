# Visual Planner & Pomodoro

A color-coded, icon-based daily timeline and a Pomodoro timer, aimed at
helping neurodivergent/ADHD users perceive time and break down a day into
visible chunks — the same goal as the existing "doubles as a low-key
calendar status display" feature, extended into a real day view instead of
just "what's next."

## What was and wasn't reused from the projects that inspired this

- **[jsoeterbroek/pictostick](https://github.com/jsoeterbroek/pictostick)**
  is a different kind of device (an M5StickC Plus2 wearable, 135x240 TFT)
  that shows a scrollable list of pictograms per activity with
  completion-checking, for autism/accessibility support. Nothing about its
  hardware, screen framework, or color/orientation handling transfers
  directly — it has none of those features — but its core idea (one icon
  per activity, check it off when done) is exactly what "icon-based
  scheduling" means here, and the vendored Lucide icon set already gives
  plenty of icon variety without needing to bring in a second icon
  library.
- **[martin-ger/esp32_nat_router](https://github.com/martin-ger/esp32_nat_router)**
  is a WiFi NAT-routing firmware with no picture/camera capability
  whatsoever — the original "pull in pictures from it" idea doesn't apply
  to anything this project actually does. Its dual AP+STA NAT-bridging
  pattern *is* reused, but as a separate, unrelated networking capability
  (opt-in hotspot bridging — see `docs/architecture.md`'s "On-device Web
  UI (opt-in)" and `docs/security.md`), not part of the planner/Pomodoro
  feature itself.

## Two surfaces, two color codings

The X4's native e-paper panel is strictly monochrome (1bpp, dithered
gray) — there's no way to render literal color there. Rather than settle
for a pattern-only approximation of "color-coded," this feature uses two
surfaces the way the rest of the project already splits native vs. web:

- **Native Timeline/Pomodoro screens** (`ui::PlannerUI`, `ui::PomodoroUI`)
  use **icon + dither-pattern coding**: each of the 8 task categories
  (`store::Category` — `Work`, `Break`, `Chore`, `Health`, `Social`,
  `School`, `Personal`, `Other`) maps to a distinct Lucide icon slug plus
  one of three dither patterns (`ui::DitherPattern::{Solid,Dense,Sparse}`,
  mirroring the existing 4x4 Bayer-dither `Black`/`DarkGray`/`LightGray`
  vocabulary in `FreeInkUIDisplayTarget.h`) — see `ui/CategoryStyle.h`'s
  mapping table. Two categories never share both an icon and a pattern, so
  they stay visually distinguishable at a glance even on a small,
  low-contrast panel.
- **The on-device Web UI's `planner.html`** page (`GET /planner`,
  `WebUiServer.cpp`) gives genuinely **color-coded** cards, using real CSS
  color from `docs/design-system.md`'s token set — this is the surface
  that actually answers "color-coded timeline," rendered in a phone
  browser over the X4's existing opt-in, PIN-gated Web UI (same session
  gate and idle-timeout teardown as `joblist.html`, no new auth
  mechanism). It's optional: the native screens are the always-available,
  fully offline experience; the web page is a nicer view when you've
  already got the Web UI turned on for something else.

The category list and its order are a cross-unit contract — both codings,
plus the Pi's `planner.CATEGORIES` tuple, must agree on the same 8 names
in the same order.

## Horizontal and vertical modes

The Timeline screen's orientation toggle (`AppSettingsData.plannerHorizontalView`,
`false`/Vertical default) follows the same precedent as the existing
**"Landscape-strip reading mode"** for documents (`docs/architecture.md`):
a per-view toggle from Settings, not a firmware-level display rotation.
Vertical stacks the day's items top-to-bottom as a day-planner ruler;
Horizontal lays them out left-to-right as a single strip using the panel's
full 800px dimension as the timeline's length instead of the shorter
480px one — same "turn the device 90°" spirit as landscape-strip reading,
applied to a schedule instead of a document page.

## The timeline merges with the existing calendar feature

The device already shows the next upcoming calendar event on the Inbox
screen when no print jobs are pending (`firmware/src/calendar/`:
`IcsParser`, `CalendarSync`, `WakeSchedule`). The Timeline screen doesn't
duplicate that — it renders **both** `store::PlannerStore`'s user-authored
tasks and the calendar module's already-synced event(s) on one merged
view, distinguished the same way task categories are (icon+pattern
natively, color on the web page). No separate fetch, no second sync
window: the calendar data the device already has is just read into the
same view.

## Tasks can be authored on the Pi, the same way calendar feeds are

Calendar feeds and Wi-Fi networks are already "managed centrally from the
Pi's admin console and pushed to every paired device." Planner tasks
follow the identical shape (`pi-server/focusink_server/planner.py`,
`docs/protocol.md` §1.8) — authored per-device/per-day on the Pi, pulled
down on the device's normal sync pass alongside jobs and calendar feeds —
rather than being on-device-only. A task marked done on the device syncs
back to the Pi via a `completion_id`-idempotent POST, the same
client-generated-idempotency-key shape `approval_id` already uses for
print approvals (`docs/protocol.md` §3).

## Pomodoro uses checkpoint wakes, not a live tick

The device's whole design center is deep sleep: it wakes only on a button
press or its own RTC timer, never remotely (`docs/architecture.md` "Deep
sleep / wake sequence"). There is no continuously-updating display mode
anywhere in this project, and a Pomodoro timer doesn't get an exception to
that. Instead, `pomodoro::PomodoroSession` uses **checkpoint wakes**:
starting a session sleeps the device as normal, and its RTC timer wakes it
every `checkpoint_minutes` (default 5) to redraw remaining time, plus once
more exactly at each phase's end — never a per-second countdown.

`PomodoroSession::secondsUntilNextCheckpoint()` is a pure function
(checkpoint boundaries anchored to the phase's start time, so a checkpoint
interval that doesn't evenly divide the phase length still lands a final
wake exactly at the phase's end rather than overshooting) composed via
`std::min` into `main.cpp`'s existing `nextWakeIntervalSeconds()`,
alongside the calendar module's own near-wake decision — one wake-timer
decision point, not two independent timers. A device that sleeps through
more than one phase boundary (e.g. a missed checkpoint wake) catches up in
a single `advance()` call on its next wake, transitioning through however
many phases are actually due rather than getting stuck.

Default config: 25 min work / 5 min break / 15 min long break / a long
break every 4 work sessions / a 5-minute checkpoint cadence
(`planner.DEFAULT_POMODORO_CONFIG`, `docs/protocol.md` §1.9) — the classic
Pomodoro technique's numbers, configurable per device from the admin
console.
