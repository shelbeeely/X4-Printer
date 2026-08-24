#pragma once
// Durable pairing config, read from /system/device.json — written onto the
// SD card by pi-server/tools/pair_device.py (see docs/setup-x4.md), never
// by the firmware itself. This is the SD-file provisioning path documented
// in docs/architecture.md; a future on-device pairing UI (QR code scan,
// etc.) would add a save() path here without changing the data shape.
//
// Small, fixed-size fields — this file is only ever a few hundred bytes,
// loaded once at boot and kept resident for the process lifetime, unlike
// the job/outbox indexes which are the things docs/architecture.md's
// "Memory budget" actually bounds.

#include <cstddef>
#include <cstdint>

namespace config {

constexpr size_t kMaxUrlLen = 128;
constexpr size_t kMaxTokenLen = 80;
constexpr size_t kMaxIdLen = 40;
constexpr size_t kMaxNameLen = 64;

constexpr const char* kDeviceConfigPath = "/system/device.json";
constexpr const char* kCaCertPath = "/system/pi_ca.pem";

struct DeviceConfigData {
  bool loaded = false;

  char deviceId[kMaxIdLen] = {0};
  char deviceToken[kMaxTokenLen] = {0};
  char deviceName[kMaxNameLen] = {0};
  char piBaseUrl[kMaxUrlLen] = {0};

  bool hasRelay = false;
  char relayBaseUrl[kMaxUrlLen] = {0};
  char relayAccountId[kMaxIdLen] = {0};
  char relayAccountToken[kMaxTokenLen] = {0};
};

class DeviceConfig {
 public:
  static DeviceConfig& instance();

  // Reads and parses /system/device.json. Returns false (and leaves
  // data().loaded == false) if the file is missing or malformed — callers
  // must treat that as "device not yet paired" and route the UI to a
  // "copy device.json to /system/ from pair_device.py" message rather than
  // crashing.
  bool load();

  const DeviceConfigData& data() const { return data_; }

 private:
  DeviceConfigData data_;
};

}  // namespace config
