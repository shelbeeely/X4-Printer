#include "config/AppSettings.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include "store/AtomicJsonFile.h"

namespace config {

AppSettings& AppSettings::instance() {
  static AppSettings inst;
  return inst;
}

void AppSettings::load() {
  data_ = AppSettingsData{};

  if (!SdMan.exists(kAppSettingsPath)) return;
  String raw = SdMan.readFile(kAppSettingsPath);
  if (raw.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return;  // malformed -- keep defaults, same as DeviceConfig/WifiStore

  data_.defaultLandscapeView = doc["default_landscape_view"] | false;
  data_.calendarWakeBeforeStart = doc["calendar_wake_before_start"] | false;
  data_.calendarWakeLeadMinutes = doc["calendar_wake_lead_minutes"] | 10;
  data_.calendarWakeAtEnd = doc["calendar_wake_at_end"] | false;
  data_.plannerHorizontalView = doc["planner_horizontal_view"] | false;
  data_.pomodoroWorkMinutes = doc["pomodoro_work_minutes"] | 25;
  data_.pomodoroBreakMinutes = doc["pomodoro_break_minutes"] | 5;
  data_.pomodoroLongBreakMinutes = doc["pomodoro_long_break_minutes"] | 15;
  data_.pomodoroSessionsBeforeLongBreak = doc["pomodoro_sessions_before_long_break"] | 4;
  data_.pomodoroCheckpointMinutes = doc["pomodoro_checkpoint_minutes"] | 5;
  data_.hotspotNatBridgeEnabled = doc["hotspot_nat_bridge_enabled"] | false;
}

bool AppSettings::save() const {
  JsonDocument doc;
  doc["default_landscape_view"] = data_.defaultLandscapeView;
  doc["calendar_wake_before_start"] = data_.calendarWakeBeforeStart;
  doc["calendar_wake_lead_minutes"] = data_.calendarWakeLeadMinutes;
  doc["calendar_wake_at_end"] = data_.calendarWakeAtEnd;
  doc["planner_horizontal_view"] = data_.plannerHorizontalView;
  doc["pomodoro_work_minutes"] = data_.pomodoroWorkMinutes;
  doc["pomodoro_break_minutes"] = data_.pomodoroBreakMinutes;
  doc["pomodoro_long_break_minutes"] = data_.pomodoroLongBreakMinutes;
  doc["pomodoro_sessions_before_long_break"] = data_.pomodoroSessionsBeforeLongBreak;
  doc["pomodoro_checkpoint_minutes"] = data_.pomodoroCheckpointMinutes;
  doc["hotspot_nat_bridge_enabled"] = data_.hotspotNatBridgeEnabled;

  String out;
  serializeJson(doc, out);
  return store::writeFileAtomic(kAppSettingsPath, out);
}

}  // namespace config
