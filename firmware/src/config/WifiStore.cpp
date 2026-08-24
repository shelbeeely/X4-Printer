#include "config/WifiStore.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <mbedtls/base64.h>

#include <cstring>

#include "store/AtomicJsonFile.h"

namespace config {

WifiStore& WifiStore::instance() {
  static WifiStore inst;
  return inst;
}

namespace {

// XOR-obfuscate against the device's own MAC — see header comment for why
// this is deliberately not "encryption." Same construction as CrossPoint's
// WifiCredentialStore.
void xorWithMac(uint8_t* data, size_t len) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  for (size_t i = 0; i < len; i++) {
    data[i] ^= mac[i % 6];
  }
}

String obfuscateAndEncode(const char* plaintext) {
  size_t len = std::strlen(plaintext);
  if (len == 0) return "";
  uint8_t buf[kMaxWifiPasswordLen];
  std::memcpy(buf, plaintext, len);
  xorWithMac(buf, len);

  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, buf, len);  // query size
  char out[128] = {0};
  if (outLen >= sizeof(out)) return "";
  size_t written = 0;
  mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), sizeof(out), &written, buf, len);
  out[written] = '\0';
  return String(out);
}

bool decodeAndDeobfuscate(const char* encoded, char* outPlain, size_t outPlainSize) {
  size_t encodedLen = std::strlen(encoded);
  if (encodedLen == 0) {
    outPlain[0] = '\0';
    return true;
  }
  uint8_t buf[kMaxWifiPasswordLen];
  size_t outLen = 0;
  int rc = mbedtls_base64_decode(buf, sizeof(buf), &outLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (rc != 0 || outLen >= outPlainSize) return false;
  xorWithMac(buf, outLen);
  std::memcpy(outPlain, buf, outLen);
  outPlain[outLen] = '\0';
  return true;
}

}  // namespace

bool WifiStore::load() {
  count_ = 0;
  lastSsid_[0] = '\0';

  if (!SdMan.exists(kWifiStorePath)) return false;
  String raw = SdMan.readFile(kWifiStorePath);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  const char* last = doc["last_connected_ssid"] | "";
  std::strncpy(lastSsid_, last, sizeof(lastSsid_) - 1);

  JsonArrayConst networks = doc["networks"].as<JsonArrayConst>();
  for (JsonObjectConst net : networks) {
    if (count_ >= kMaxWifiNetworks) break;
    const char* ssid = net["ssid"] | "";
    const char* encodedPassword = net["password"] | "";
    if (ssid[0] == '\0') continue;

    WifiCredential& cred = creds_[count_];
    std::strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
    if (!decodeAndDeobfuscate(encodedPassword, cred.password, sizeof(cred.password))) {
      cred.password[0] = '\0';
    }
    count_++;
  }
  return true;
}

bool WifiStore::save() const {
  JsonDocument doc;
  doc["last_connected_ssid"] = lastSsid_;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (size_t i = 0; i < count_; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = creds_[i].ssid;
    net["password"] = obfuscateAndEncode(creds_[i].password);
  }

  String out;
  serializeJson(doc, out);
  return store::writeFileAtomic(kWifiStorePath, out);
}

bool WifiStore::addOrUpdate(const char* ssid, const char* password) {
  for (size_t i = 0; i < count_; i++) {
    if (std::strncmp(creds_[i].ssid, ssid, kMaxSsidLen) == 0) {
      std::strncpy(creds_[i].password, password, sizeof(creds_[i].password) - 1);
      return true;
    }
  }
  if (count_ >= kMaxWifiNetworks) return false;
  std::strncpy(creds_[count_].ssid, ssid, sizeof(creds_[count_].ssid) - 1);
  std::strncpy(creds_[count_].password, password, sizeof(creds_[count_].password) - 1);
  count_++;
  return true;
}

bool WifiStore::remove(const char* ssid) {
  for (size_t i = 0; i < count_; i++) {
    if (std::strncmp(creds_[i].ssid, ssid, kMaxSsidLen) == 0) {
      creds_[i] = creds_[count_ - 1];
      count_--;
      return true;
    }
  }
  return false;
}

const WifiCredential* WifiStore::find(const char* ssid) const {
  for (size_t i = 0; i < count_; i++) {
    if (std::strncmp(creds_[i].ssid, ssid, kMaxSsidLen) == 0) return &creds_[i];
  }
  return nullptr;
}

void WifiStore::setLastConnectedSsid(const char* ssid) {
  std::strncpy(lastSsid_, ssid, sizeof(lastSsid_) - 1);
}

}  // namespace config
