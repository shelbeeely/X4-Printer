#pragma once
// On-device-editable app preferences, read from and written to
// /system/app_settings.json. Distinct from DeviceConfig (pi-provisioned,
// read-only, never written by firmware — see DeviceConfig.h) and WifiStore
// (its own file, its own obfuscation rules for passwords): this is the
// small set of display/behavior preferences a user sets from the on-device
// Settings screen (ui/InboxUI.cpp) with nothing sync-protocol-relevant in
// it, so it deliberately doesn't share a file with either.
//
// Same "freestanding data, unload() on missing/corrupt file" convention as
// DeviceConfig — a missing or unparseable file is "use defaults", not an
// error, so a fresh SD card or hand-edited file never blocks boot.

#include <cstddef>
#include <cstdint>

namespace config {

constexpr const char* kAppSettingsPath = "/system/app_settings.json";

struct AppSettingsData {
  // Applied as the initial view mode every time a document is opened
  // (ui/InboxUI.cpp's ActionOpenJob handler) -- only takes effect for jobs
  // that actually have a landscape-strip variant (docs/protocol.md §4);
  // jobs without one always open portrait regardless of this setting.
  bool defaultLandscapeView = false;

  // Calendar wake reminders (Settings > Calendar tab; see
  // calendar/WakeSchedule.h) -- both independently opt-in and off by
  // default, since neither means anything until /system/calendars.json is
  // configured (docs/setup-x4.md "Calendar idle screen").
  bool calendarWakeBeforeStart = false;
  uint16_t calendarWakeLeadMinutes = 10;
  bool calendarWakeAtEnd = false;

  // Timeline screen orientation (ui/PlannerUI.h, docs/planner.md) -- same
  // per-view-toggle-not-firmware-rotation shape as defaultLandscapeView
  // above, following that field's "Landscape-strip reading mode"
  // precedent. false = vertical (default): a day-planner ruler,
  // top-to-bottom. true = horizontal: a single left-to-right strip using
  // the panel's full width as the timeline's length.
  bool plannerHorizontalView = false;
};

class AppSettings {
 public:
  static AppSettings& instance();

  // Reads /system/app_settings.json. Always leaves data() in a valid state
  // (defaults on a missing or malformed file) -- callers never need to
  // branch on the return value the way DeviceConfig::load()'s callers do,
  // since there's no "not configured yet" state to distinguish here.
  void load();
  bool save() const;

  const AppSettingsData& data() const { return data_; }
  void setDefaultLandscapeView(bool value) { data_.defaultLandscapeView = value; }
  void setCalendarWakeBeforeStart(bool value) { data_.calendarWakeBeforeStart = value; }
  void setCalendarWakeLeadMinutes(uint16_t value) { data_.calendarWakeLeadMinutes = value; }
  void setCalendarWakeAtEnd(bool value) { data_.calendarWakeAtEnd = value; }
  void setPlannerHorizontalView(bool value) { data_.plannerHorizontalView = value; }

 private:
  AppSettingsData data_;
};

}  // namespace config
