#include "calendar/WakeSchedule.h"

namespace calendar {

namespace {

// A computed sleepSeconds this small would wake the device again almost
// immediately (e.g. `now` landing exactly on a target, or clock skew
// between the target and this call) -- floor it so a pathological
// lead-time/event-time combination can't spin the device in a
// wake-sleep-wake loop.
constexpr uint32_t kMinSleepSeconds = 30;

}  // namespace

WakeDecision computeWakeDecision(const config::AppSettingsData& settings, config::NextEventInfo& event, time_t now,
                                  uint32_t baseIntervalSeconds) {
  WakeDecision decision;
  decision.sleepSeconds = baseIntervalSeconds;

  if (!event.hasEvent) return decision;

  bool beforeStartFired = false;
  bool atEndFired = false;

  if (settings.calendarWakeBeforeStart) {
    time_t target = event.start - static_cast<time_t>(settings.calendarWakeLeadMinutes) * 60;
    if (event.alertedForStart != target) {
      if (now >= target) {
        beforeStartFired = true;
        event.alertedForStart = target;
      } else {
        uint32_t untilTarget = static_cast<uint32_t>(target - now);
        if (untilTarget < decision.sleepSeconds) decision.sleepSeconds = untilTarget;
      }
    }
  }

  if (settings.calendarWakeAtEnd) {
    time_t target = event.end;
    if (event.alertedForEnd != target) {
      if (now >= target) {
        atEndFired = true;
        event.alertedForEnd = target;
      } else {
        uint32_t untilTarget = static_cast<uint32_t>(target - now);
        if (untilTarget < decision.sleepSeconds) decision.sleepSeconds = untilTarget;
      }
    }
  }

  if (beforeStartFired) {
    decision.alert = WakeAlertKind::BeforeStart;
  } else if (atEndFired) {
    decision.alert = WakeAlertKind::AtEnd;
  }

  if (decision.sleepSeconds < kMinSleepSeconds) decision.sleepSeconds = kMinSleepSeconds;
  return decision;
}

}  // namespace calendar
