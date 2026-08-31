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
}

bool AppSettings::save() const {
  JsonDocument doc;
  doc["default_landscape_view"] = data_.defaultLandscapeView;

  String out;
  serializeJson(doc, out);
  return store::writeFileAtomic(kAppSettingsPath, out);
}

}  // namespace config
