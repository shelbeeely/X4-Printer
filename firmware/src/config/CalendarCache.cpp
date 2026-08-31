#include "config/CalendarCache.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

#include "store/AtomicJsonFile.h"

namespace config {

CalendarCache& CalendarCache::instance() {
  static CalendarCache inst;
  return inst;
}

void CalendarCache::load() {
  data_ = NextEventInfo{};

  if (!SdMan.exists(kCalendarCachePath)) return;
  String raw = SdMan.readFile(kCalendarCachePath);
  if (raw.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return;

  if (!(doc["has_event"] | false)) return;
  data_.hasEvent = true;
  std::strncpy(data_.title, doc["title"] | "", sizeof(data_.title) - 1);
  data_.start = static_cast<time_t>(doc["start"] | 0);
  data_.end = static_cast<time_t>(doc["end"] | 0);
  data_.allDay = doc["all_day"] | false;
  data_.lastSyncedAt = static_cast<time_t>(doc["last_synced_at"] | 0);
}

bool CalendarCache::save() const {
  JsonDocument doc;
  doc["has_event"] = data_.hasEvent;
  doc["title"] = data_.title;
  doc["start"] = static_cast<int64_t>(data_.start);
  doc["end"] = static_cast<int64_t>(data_.end);
  doc["all_day"] = data_.allDay;
  doc["last_synced_at"] = static_cast<int64_t>(data_.lastSyncedAt);

  String out;
  serializeJson(doc, out);
  return store::writeFileAtomic(kCalendarCachePath, out);
}

}  // namespace config
