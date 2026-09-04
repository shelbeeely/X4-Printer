#pragma once
// Pure, host-testable merge of store::TaskIndex (user-authored planner
// tasks) with the existing calendar module's cached next event
// (config::NextEventInfo) into one time-sorted list for the Timeline
// screen (ui/PlannerUI.h) -- header-only, no FreeInkUI/Arduino dependency,
// same split rationale as calendar/WakeSchedule.h: the sort/merge logic
// that actually matters for correctness stays testable off-device, the
// FreeInkUI rendering that consumes it doesn't need to be.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config/CalendarCache.h"
#include "store/PlannerStore.h"

namespace ui {

constexpr size_t kMaxTimelineItems = store::kMaxPlannerTasks + 1;  // +1 for the single calendar next-event
constexpr size_t kTimelineLabelLen = 96;

struct TimelineItem {
  char label[kTimelineLabelLen] = {0};  // "HH:MM-HH:MM Title" (or "Next: HH:MM Title" for the calendar row)
  store::Category category = store::Category::Other;
  bool isCalendarEvent = false;
  bool done = false;
};

// Formats a calendar event's UTC time_t as "HH:MM" -- same UTC caveat
// ui/InboxUI.cpp's idleScreenMessage() already documents (this firmware
// never configures a timezone).
inline void formatUtcHm(time_t t, char* out, size_t outLen) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  std::strftime(out, outLen, "%H:%M", &tmv);
}

// Builds a time-sorted merge of `tasks` (already-parsed HH:MM strings) and
// `nextEvent` (0 or 1 calendar row) into `out` (capacity `maxItems`).
// Insertion-sorted by start-time string (tasks' "HH:MM" and the formatted
// calendar time compare correctly as plain strings) -- task counts here
// are small (kMaxPlannerTasks) so this is never a bottleneck. Returns the
// number of items written.
//
// No timezone conversion happens here: both inputs are plain "HH:MM"
// wall-clock strings compared as-is, matching this firmware's existing
// no-timezone-support limitation (see store::TaskEntry::startTime's
// comment and ui/InboxUI.cpp's idleScreenMessage()). On a device actually
// used across timezones, a calendar event's UTC time and a task's
// as-entered time are not guaranteed to be the same clock, so their
// relative order here can be wrong -- a real fix needs this firmware to
// track a UTC offset at all, which it doesn't do anywhere today.
inline size_t buildTimelineItems(const store::TaskIndex& tasks, const config::NextEventInfo* nextEvent,
                                  TimelineItem* out, size_t maxItems) {
  size_t count = 0;

  auto insertSorted = [&](const TimelineItem& item) {
    if (count >= maxItems) return;
    size_t pos = count;
    while (pos > 0 && std::strncmp(out[pos - 1].label, item.label, 5) > 0) {
      out[pos] = out[pos - 1];
      pos--;
    }
    out[pos] = item;
    count++;
  };

  for (size_t i = 0; i < tasks.count() && count < maxItems; i++) {
    const store::TaskEntry& t = tasks.at(i);
    TimelineItem item;
    std::snprintf(item.label, sizeof(item.label), "%s-%s %s", t.startTime, t.endTime, t.title);
    item.category = t.category;
    item.isCalendarEvent = false;
    item.done = t.done;
    insertSorted(item);
  }

  if (nextEvent != nullptr && nextEvent->hasEvent && count < maxItems) {
    char hm[6];
    formatUtcHm(nextEvent->start, hm, sizeof(hm));
    TimelineItem item;
    // "HH:MM" prefix (5 chars) matches task labels' own prefix width, so
    // insertSorted's 5-char compare places this row correctly relative to
    // tasks regardless of insertion order.
    std::snprintf(item.label, sizeof(item.label), "%s Cal: %s", hm, nextEvent->title);
    item.category = store::Category::Other;  // no dedicated category -- distinguished via isCalendarEvent instead
    item.isCalendarEvent = true;
    item.done = false;
    insertSorted(item);
  }

  return count;
}

}  // namespace ui
