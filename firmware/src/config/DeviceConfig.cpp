#include "config/DeviceConfig.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

namespace config {

DeviceConfig& DeviceConfig::instance() {
  static DeviceConfig inst;
  return inst;
}

namespace {
void copyField(char* dst, size_t dstSize, JsonVariantConst v) {
  const char* s = v | "";
  std::strncpy(dst, s, dstSize - 1);
  dst[dstSize - 1] = '\0';
}
}  // namespace

bool DeviceConfig::load() {
  data_ = DeviceConfigData{};

  if (!SdMan.exists(kDeviceConfigPath)) {
    return false;
  }
  String raw = SdMan.readFile(kDeviceConfigPath);
  if (raw.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    return false;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root["device_id"].is<const char*>() || !root["device_token"].is<const char*>() ||
      !root["pi_base_url"].is<const char*>()) {
    return false;  // the three required fields, see docs/protocol.md §1
  }

  copyField(data_.deviceId, sizeof(data_.deviceId), root["device_id"]);
  copyField(data_.deviceToken, sizeof(data_.deviceToken), root["device_token"]);
  copyField(data_.piBaseUrl, sizeof(data_.piBaseUrl), root["pi_base_url"]);
  copyField(data_.deviceName, sizeof(data_.deviceName), root["device_name"]);

  if (root["relay_base_url"].is<const char*>() && root["relay_account_id"].is<const char*>() &&
      root["relay_account_token"].is<const char*>()) {
    data_.hasRelay = true;
    copyField(data_.relayBaseUrl, sizeof(data_.relayBaseUrl), root["relay_base_url"]);
    copyField(data_.relayAccountId, sizeof(data_.relayAccountId), root["relay_account_id"]);
    copyField(data_.relayAccountToken, sizeof(data_.relayAccountToken), root["relay_account_token"]);
  }

  data_.loaded = true;
  return true;
}

}  // namespace config
