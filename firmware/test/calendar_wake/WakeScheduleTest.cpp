#include "testutil.h"

#include "calendar/WakeSchedule.h"
#include "config/AppSettings.h"
#include "config/CalendarCache.h"

using calendar::computeWakeDecision;
using calendar::WakeAlertKind;
using config::AppSettingsData;
using config::NextEventInfo;

namespace {

constexpr uint32_t kBaseInterval = 3600;
constexpr time_t kNow = 2000000000;

NextEventInfo makeEvent(time_t start, time_t end) {
  NextEventInfo event;
  event.hasEvent = true;
  event.start = start;
  event.end = end;
  return event;
}

}  // namespace

int main() {
  // No event configured: always the base interval, never an alert.
  {
    AppSettingsData settings;
    settings.calendarWakeBeforeStart = true;
    settings.calendarWakeAtEnd = true;
    NextEventInfo event;  // hasEvent == false
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::None);
    CHECK(decision.sleepSeconds == kBaseInterval);
  }

  // Both triggers off: event present but nothing armed -- base interval,
  // no alert, no mutation.
  {
    AppSettingsData settings;  // both false by default
    NextEventInfo event = makeEvent(kNow + 3000, kNow + 6000);
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::None);
    CHECK(decision.sleepSeconds == kBaseInterval);
    CHECK(event.alertedForStart == 0);
    CHECK(event.alertedForEnd == 0);
  }

  // Before-start armed, target still in the future: sleeps exactly until
  // the target (shorter than the base interval), no alert yet.
  {
    AppSettingsData settings;
    settings.calendarWakeBeforeStart = true;
    settings.calendarWakeLeadMinutes = 10;  // 600s
    NextEventInfo event = makeEvent(kNow + 1000, kNow + 5000);  // target = now + 400
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::None);
    CHECK(decision.sleepSeconds == 400);
    CHECK(event.alertedForStart == 0);
  }

  // Before-start armed, target already reached: fires once and stamps the
  // dedup marker.
  {
    AppSettingsData settings;
    settings.calendarWakeBeforeStart = true;
    settings.calendarWakeLeadMinutes = 10;
    NextEventInfo event = makeEvent(kNow + 500, kNow + 5000);  // target = now - 100, already due
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::BeforeStart);
    CHECK(event.alertedForStart == kNow + 500 - 600);

    // Calling again for the same (now-mutated) event never refires it.
    calendar::WakeDecision again = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(again.alert == WakeAlertKind::None);
    CHECK(again.sleepSeconds == kBaseInterval);
  }

  // At-end armed, already past the event's end: fires and stamps
  // alertedForEnd, independent of alertedForStart.
  {
    AppSettingsData settings;
    settings.calendarWakeAtEnd = true;
    NextEventInfo event = makeEvent(kNow - 5000, kNow - 10);  // ended 10s ago
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::AtEnd);
    CHECK(event.alertedForEnd == kNow - 10);
    CHECK(event.alertedForStart == 0);  // untouched -- not armed
  }

  // Both armed, before-start due now and at-end still ahead: reports
  // BeforeStart (priority order) and sleeps until the still-pending
  // at-end target, not the base interval.
  {
    AppSettingsData settings;
    settings.calendarWakeBeforeStart = true;
    settings.calendarWakeLeadMinutes = 0;  // target == start
    settings.calendarWakeAtEnd = true;
    NextEventInfo event = makeEvent(kNow, kNow + 900);  // start due exactly now, end in 900s
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::BeforeStart);
    CHECK(decision.sleepSeconds == 900);
    CHECK(event.alertedForStart == kNow);
    CHECK(event.alertedForEnd == 0);  // still pending, not fired

    // Next wake, once "now" reaches the end target: fires AtEnd, and the
    // already-fired start target never refires.
    calendar::WakeDecision atEnd = computeWakeDecision(settings, event, kNow + 900, kBaseInterval);
    CHECK(atEnd.alert == WakeAlertKind::AtEnd);
    CHECK(event.alertedForEnd == kNow + 900);
  }

  // A target due only a couple of seconds from now clamps to the minimum
  // sleep floor rather than scheduling a near-immediate re-wake.
  {
    AppSettingsData settings;
    settings.calendarWakeBeforeStart = true;
    settings.calendarWakeLeadMinutes = 0;
    NextEventInfo event = makeEvent(kNow + 2, kNow + 100);
    calendar::WakeDecision decision = computeWakeDecision(settings, event, kNow, kBaseInterval);
    CHECK(decision.alert == WakeAlertKind::None);
    CHECK(decision.sleepSeconds == 30);
  }

  std::printf("WakeScheduleTest: all checks passed\n");
  return 0;
}
