#pragma once
// Durable Wi-Fi credential storage, patterned directly on CrossPoint
// Reader's WifiCredentialStore (src/WifiCredentialStore.h in
// crosspoint-reader): JSON on SD at a fixed path, atomic write, and
// passwords XOR-obfuscated against the device's own MAC address before
// being base64-encoded to disk — explicitly NOT cryptographically secure
// (documented in CrossPoint's own header the same way), just enough to
// keep a saved Wi-Fi password from being plaintext-grep-able off a
// removed SD card. Real confidentiality of the password matters far less
// here than it does for CrossPoint's arbitrary-network use case, since this
// firmware only ever joins networks the owner already configured for the
// same household's Pi — but there's no reason to regress below what the
// reference project already does for the same file class.

#include <cstddef>
#include <cstdint>

namespace config {

constexpr size_t kMaxWifiNetworks = 8;
constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxWifiPasswordLen = 64;

constexpr const char* kWifiStorePath = "/system/wifi.json";

struct WifiCredential {
  char ssid[kMaxSsidLen + 1] = {0};
  char password[kMaxWifiPasswordLen + 1] = {0};  // plaintext once loaded into RAM
};

class WifiStore {
 public:
  static WifiStore& instance();

  bool load();
  bool save() const;

  // Inserts or updates (by ssid). Returns false if the store is full and
  // ssid is new.
  bool addOrUpdate(const char* ssid, const char* password);
  bool remove(const char* ssid);

  size_t count() const { return count_; }
  const WifiCredential& at(size_t i) const { return creds_[i]; }
  const WifiCredential* find(const char* ssid) const;

  const char* lastConnectedSsid() const { return lastSsid_; }
  void setLastConnectedSsid(const char* ssid);

 private:
  WifiCredential creds_[kMaxWifiNetworks];
  size_t count_ = 0;
  char lastSsid_[kMaxSsidLen + 1] = {0};
};

}  // namespace config
