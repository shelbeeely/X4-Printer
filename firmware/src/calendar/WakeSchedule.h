#pragma once
// Pure logic (no Arduino/hardware deps -- host-testable, see
// firmware/test/calendar_wake/WakeScheduleTest.cpp) for combining the
// regular background-sync timer interval with calendar-event wake targets:
// "wake N minutes before the next event starts" and/or "wake when it
// ends" (Settings > Calendar tab, config::AppSettingsData). Each target
// fires at most once per event -- see NextEventInfo::alertedForStart/
// alertedForEnd's comment in config/CalendarCache.h for how the dedup
// survives the deep-sleep cycle.
//
// Deliberately takes `now` and `baseIntervalSeconds` as parameters rather
// than reading time(nullptr) or a compile-time constant internally, so
// this stays pure and testable off-device.

#include <cstdint>
#include <ctime>

#include "config/AppSettings.h"
#include "config/CalendarCache.h"

namespace calendar {

enum class WakeAlertKind : uint8_t {
  None,
  BeforeStart,
  AtEnd,
};

struct WakeDecision {
  // WakeAlertKind::None means "nothing to show this wake" -- just sleep
  // for sleepSeconds. A non-None value means the caller should render one
  // reminder frame before sleeping (main.cpp's setup()).
  WakeAlertKind alert = WakeAlertKind::None;
  // Seconds until the next thing that needs this device awake -- either
  // the regular background sync interval, or a sooner not-yet-fired
  // calendar wake target, whichever is smaller. Always >= 1.
  uint32_t sleepSeconds = 0;
};

// Mutates `event`: when a target is due (now >= target), its matching
// alertedForStart/alertedForEnd field is stamped with that target so a
// later call for the same event (same start/end) never fires it again --
// the caller is responsible for persisting `event` (via
// config::CalendarCache::instance().set()/save()) whenever alert != None,
// so a crash between here and the next sync can't re-fire it. Passing a
// scratch copy of a NextEventInfo (rather than the live one) computes
// sleepSeconds without committing to firing anything -- see main.cpp's
// nextWakeIntervalSeconds().
//
// If both targets are due on the exact same call, only BeforeStart is
// reported as the alert to show (AtEnd is still marked fired, silently
// skipped) -- an edge case only reachable when the lead time is at least
// as long as the event itself.
WakeDecision computeWakeDecision(const config::AppSettingsData& settings, config::NextEventInfo& event, time_t now,
                                  uint32_t baseIntervalSeconds);

}  // namespace calendar
