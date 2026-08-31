#include "config/CalendarConfig.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

namespace config {

CalendarConfig& CalendarConfig::instance() {
  static CalendarConfig inst;
  return inst;
}

void CalendarConfig::load() {
  count_ = 0;
  for (auto& feed : feeds_) feed = CalendarFeed{};

  if (!SdMan.exists(kCalendarConfigPath)) return;
  String raw = SdMan.readFile(kCalendarConfigPath);
  if (raw.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return;  // malformed -- keep empty, same as DeviceConfig/WifiStore

  JsonArrayConst calendars = doc["calendars"].as<JsonArrayConst>();
  for (JsonObjectConst cal : calendars) {
    if (count_ >= kMaxCalendars) break;
    const char* url = cal["url"] | "";
    if (url[0] == '\0') continue;
    CalendarFeed& feed = feeds_[count_];
    std::strncpy(feed.url, url, sizeof(feed.url) - 1);
    std::strncpy(feed.label, cal["label"] | "", sizeof(feed.label) - 1);
    count_++;
  }
}

}  // namespace config
