# TimeBoard

A small, portable ADHD/time-blindness tool: a color-coded, icon-based
visual timeline for a day's plan, plus a Pomodoro timer, on a
[M5StickC Plus2](https://docs.m5stack.com/en/core/M5StickC%20PLUS2)
(ESP32 + 1.14" color TFT).

Time-blindness, executive dysfunction, and task overwhelm are common
ADHD/neurodivergent-brain challenges; the idea here (like a lot of ADHD
tooling) is to make time and tasks *visible* rather than something you
have to hold in your head — a glanceable strip of color you can look at
instead of a clock you have to interpret.

## Where this idea comes from

Inspired by [PictoStick](https://github.com/jsoeterbroek/pictostick) — a
portable pictogram board (also M5StickC-family hardware) that shows a
scrollable sequence of activity icons for autism/mental-disability
support, with check-off and caregiver web configuration. TimeBoard shares
the "small portable ESP32 device showing icons for daily activities"
idea and the general hardware family, but is a **from-scratch
implementation**, not a fork — no PictoStick code, icon assets, or web
config server are reused. What's different:

| | PictoStick | TimeBoard |
|---|---|---|
| Layout | Scrollable list, one activity at a time | A single color-coded timeline strip showing the *whole* day/session at once |
| Orientation | Fixed | Horizontal or vertical (toggle) |
| Time awareness | Sequence only — no sense of *duration* or *time remaining* | Segment width/height is proportional to planned duration; a "now" marker moves across it |
| Focus tool | None | Built-in Pomodoro timer |
| Icons | ~150 real Material Design pictograms | Text glyphs for now — see "Known limitations" |
| Configuration | Web UI for caregivers | None yet — see "Known limitations" |

## Features

- **Color-coded timeline** (`src/ui/TimelineView.*`) — the whole screen
  is divided into segments, one per task, sized proportional to its
  planned duration and filled with that task's color, with a moving
  "now" marker.
- **Icon-based scheduling** (`src/ui/IconSet.*`) — every task carries an
  icon id alongside its color and duration.
- **Horizontal / vertical orientation** (`src/orientation/Orientation.h`)
  — the same timeline renders left-to-right or top-to-bottom; toggled
  with a button press. PictoStick has neither mode; this is new.
- **Pomodoro timer** (`src/pomodoro/PomodoroTimer.*`) — standard work/
  short-break/long-break cycle, pause/resume without losing progress,
  full-screen countdown view (`src/ui/PomodoroView.*`).
- **Task breakdown** (`model::Task::subtasks`) — a task can carry a
  handful of sub-steps, so a big task isn't one overwhelming block.
- **Focus lock** — while a Pomodoro *work* phase is running unpaused,
  switching to the timeline screen auto-pauses the session first (see
  `src/main.cpp`'s `handleButtons()`), rather than letting a "quick
  peek" run down the timer unattended, or blocking the switch outright
  with no way to check the schedule.
- **Durable, atomic storage** (`src/store/TaskStore.*`) — the schedule is
  saved to the device's internal flash (LittleFS; this board has no SD
  slot) using the same write-then-rename discipline as the main
  X4-Printer firmware's `AtomicJsonFile`, so a crash or power loss
  mid-save can't corrupt the saved schedule.

## Hardware

- **M5StickC Plus2** — ESP32, 135x240 IPS color TFT, buttons (`BtnA`,
  `BtnB`, `BtnPWR`), no SD slot.
- Board id/library versions in `platformio.ini` are verified against
  [renehagen/M5StickC-PLUS2-platformio-example](https://github.com/renehagen/M5StickC-PLUS2-platformio-example),
  not against real hardware — see "Known limitations."

## Controls

| Button | Timeline screen | Pomodoro screen |
|---|---|---|
| `BtnA` (front) | Start/pause Pomodoro | Start/pause Pomodoro |
| `BtnA` held | Reset Pomodoro | Reset Pomodoro |
| `BtnB` (side) | Toggle horizontal/vertical | Toggle horizontal/vertical (takes effect once back on the Timeline screen) |
| `BtnPWR` (power) | Switch to Pomodoro screen | Switch to Timeline screen (auto-pauses an active work session first) |

## Build / flash

```sh
cd timeboard
pio run              # build
pio run -t upload    # flash
```

## Host-side unit tests

Pure-logic modules (`model::Schedule`, `pomodoro::PomodoroTimer`,
`orientation::toggle`) have no Arduino/M5Unified dependency and build/run
on any machine with a C++17 compiler + CMake — same pattern as the main
X4-Printer firmware's `firmware/test/`:

```sh
cd timeboard/test
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

## Known limitations

This was built and verified with host-side unit tests only — there is no
M5StickC Plus2 hardware available in this development environment to
build-and-flash-verify the actual firmware (`platformio.ini`'s board id
and PlatformIO library versions, and all of `src/ui/*`, `src/main.cpp`,
and `src/store/TaskStore.cpp`'s LittleFS calls, are unverified against
real hardware). Before relying on this device, flash it and check that
it builds, boots, and renders as expected.

Beyond that:

- **Icons are text glyphs, not pictograms.** `ui/IconSet.cpp` returns
  short words ("Eat", "Zzz", "Study") drawn on the task's color, not real
  icon artwork like PictoStick's Material Design set. Converting a
  licensed icon pack into RGB565 bitmaps for M5GFX, and picking a legible
  size, is real work that has no way to be visually verified without the
  hardware — `IconSet::glyphFor()` is the one place that would change to
  add real bitmaps later.
- **No on-device task editor.** The schedule is seeded once from
  `main.cpp`'s `seedDemoSchedule()` and then persisted; to change tasks
  today, edit that function and reflash. `store::TaskStore` already
  supports arbitrary schedules — a button-driven or (PictoStick-style)
  web-based editor is a natural next step, not a redesign.
- **No wall clock.** "Elapsed time into the schedule" is measured from
  device boot (`millis()`), not a real time-of-day — there's no RTC or
  NTP sync yet, so the "now" marker restarts at the beginning of the
  timeline every time the device reboots.
- **No accelerometer-based auto-rotation**, even though the Plus2 has a
  built-in IMU — orientation is a manual button toggle for now
  (`src/orientation/Orientation.h`).
