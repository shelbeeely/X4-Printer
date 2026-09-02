#pragma once
// Calendar feed list, read from /system/calendars.json. Two ways this file
// gets written: by hand for first boot / bootstrap (same story WifiStore.h
// documents for wifi.json -- see docs/setup-x4.md), or -- the primary path
// once a device is paired -- pulled from the Pi's admin console and
// written by SyncManager on every sync (docs/protocol.md §1.6). Either
// way this device never has an on-device "add a calendar" flow of its own
// (ICS feed URLs are long and often carry an auth token in the query
// string, and this device has no keyboard).

#include <cstddef>

namespace config {

constexpr size_t kMaxCalendars = 4;
constexpr size_t kMaxCalendarUrlLen = 220;
constexpr size_t kMaxCalendarLabelLen = 32;

constexpr const char* kCalendarConfigPath = "/system/calendars.json";

struct CalendarFeed {
  char url[kMaxCalendarUrlLen + 1] = {0};
  char label[kMaxCalendarLabelLen + 1] = {0};  // optional display name, e.g. "Work"
};

class CalendarConfig {
 public:
  static CalendarConfig& instance();

  // Reads /system/calendars.json. A missing or malformed file leaves
  // count() == 0 (not an error) -- calendar sync is opt-in, same as the
  // relay: absent config means "feature not in use," not a fault.
  void load();
  bool save() const;

  // Wholesale replace, not a merge -- called by SyncManager with the Pi's
  // synced list (docs/protocol.md §1.6). Unlike WifiStore's addOrUpdate
  // merge, a full replace here is safe: there's no on-device way to add a
  // calendar independently of the Pi, so nothing local is ever at risk of
  // being clobbered. Truncates to kMaxCalendars if `count` is larger.
  void replaceAll(const CalendarFeed* feeds, size_t count);

  size_t count() const { return count_; }
  const CalendarFeed& at(size_t i) const { return feeds_[i]; }

 private:
  CalendarFeed feeds_[kMaxCalendars];
  size_t count_ = 0;
};

}  // namespace config
