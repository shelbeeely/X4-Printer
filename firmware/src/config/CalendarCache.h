#pragma once
// Persisted "next upcoming event" -- the one thing calendar sync actually
// needs to survive past the sync pass that computed it. Written to
// /system/calendar_cache.json (atomic write, same helper WifiStore/
// JobStore use) after every sync pass that reached at least one calendar
// successfully; left untouched on a wake where Wi-Fi/every feed fails, so
// the idle screen shows the last known next event (clearly stale, per
// lastSyncedAt) instead of going blank over a transient network hiccup.
//
// Deliberately just one event, not a small agenda: this firmware shows a
// single "next event" widget on the Inbox screen when there are no print
// jobs (see ui/InboxUI.cpp), not a calendar browser.

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace config {

constexpr const char* kCalendarCachePath = "/system/calendar_cache.json";
constexpr size_t kCalendarEventTitleLen = 80;

struct NextEventInfo {
  bool hasEvent = false;
  char title[kCalendarEventTitleLen + 1] = {0};
  time_t start = 0;
  time_t end = 0;
  bool allDay = false;
  // When this record was produced (a successful sync pass), so the idle
  // screen can show "as of ..." for a stale cache rather than presenting
  // it as live.
  time_t lastSyncedAt = 0;
};

class CalendarCache {
 public:
  static CalendarCache& instance();

  // Missing/malformed file means "no cached event yet" (hasEvent stays
  // false) -- not an error, same convention as every other SD-backed
  // store in this firmware.
  void load();
  bool save() const;

  const NextEventInfo& data() const { return data_; }
  void set(const NextEventInfo& info) { data_ = info; }

 private:
  NextEventInfo data_;
};

}  // namespace config
