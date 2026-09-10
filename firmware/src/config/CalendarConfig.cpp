#include "config/CalendarConfig.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

#include "store/AtomicJsonFile.h"

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

bool CalendarConfig::save() const {
  JsonDocument doc;
  JsonArray calendars = doc["calendars"].to<JsonArray>();
  for (size_t i = 0; i < count_; i++) {
    JsonObject cal = calendars.add<JsonObject>();
    cal["url"] = feeds_[i].url;
    cal["label"] = feeds_[i].label;
  }

  String out;
  serializeJson(doc, out);
  return store::writeFileAtomic(kCalendarConfigPath, out);
}

void CalendarConfig::replaceAll(const CalendarFeed* feeds, size_t count) {
  count_ = 0;
  for (auto& feed : feeds_) feed = CalendarFeed{};
  for (size_t i = 0; i < count && count_ < kMaxCalendars; i++) {
    feeds_[count_] = feeds[i];
    count_++;
  }
}

}  // namespace config
