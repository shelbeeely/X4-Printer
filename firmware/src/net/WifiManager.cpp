#include "net/WifiManager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config/WifiStore.h"

namespace net {

namespace {

// Shared by connect() and connectKeepingAp() -- identical scan/match/
// connect logic, differing only in which WiFi mode is set beforehand
// (WIFI_STA replaces any existing AP; WIFI_AP_STA preserves one).
bool connectWithMode(wifi_mode_t mode, uint32_t timeoutMs) {
  config::WifiStore& store = config::WifiStore::instance();
  if (store.count() == 0) {
    return false;  // nothing saved yet — normal on a freshly-provisioned device
  }

  WiFi.mode(mode);
  WiFi.disconnect(false);
  delay(50);

  int found = WiFi.scanNetworks();
  if (found <= 0) {
    return false;
  }

  const config::WifiCredential* best = nullptr;
  int32_t bestRssi = INT32_MIN;
  for (int i = 0; i < found; i++) {
    const config::WifiCredential* cred = store.find(WiFi.SSID(i).c_str());
    if (cred == nullptr) continue;
    int32_t rssi = WiFi.RSSI(i);
    if (rssi > bestRssi) {
      bestRssi = rssi;
      best = cred;
    }
  }
  WiFi.scanDelete();

  if (best == nullptr) {
    return false;  // saved networks exist, but none are visible right now
  }

  WiFi.begin(best->ssid, best->password);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) {
      if (mode == WIFI_AP_STA) {
        // wifioff=true (the else branch) would call esp_wifi_stop() and
        // take the just-started softAP down with it -- WifiManager owns
        // the AP_STA -> AP teardown shape in one place (see
        // disconnectKeepingAp()) rather than duplicating it here and in
        // net::NatBridge.
        WifiManager().disconnectKeepingAp();
      } else {
        WiFi.disconnect(true);
      }
      return false;
    }
    delay(100);
  }

  store.setLastConnectedSsid(best->ssid);
  store.save();
  return true;
}

}  // namespace

bool WifiManager::connect(uint32_t timeoutMs) { return connectWithMode(WIFI_STA, timeoutMs); }

bool WifiManager::connectKeepingAp(uint32_t timeoutMs) { return connectWithMode(WIFI_AP_STA, timeoutMs); }

void WifiManager::disconnect() {
  // wifioff=true, eraseap=true: also clears the ESP32 WiFi driver's own NVS
  // AP record. Safe — this firmware never relies on the driver's built-in
  // auto-reconnect; connect() always re-supplies credentials from
  // config::WifiStore explicitly on every wake.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

void WifiManager::disconnectKeepingAp() {
  // Mirrors the timeout branch's AP_STA -> AP transition above: drop the
  // STA half only (wifioff=false), then restore AP-only mode so the
  // softAP interface itself is untouched.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_AP);
}

bool WifiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }

String WifiManager::currentSsid() const { return WiFi.SSID(); }

String WifiManager::currentIp() const { return WiFi.localIP().toString(); }

int32_t WifiManager::rssi() const { return WiFi.RSSI(); }

bool WifiManager::startAccessPoint(const char* ssid, const char* password) {
  WiFi.mode(WIFI_AP);
  // softAP() requires an 8+ character password (or empty for an open
  // network, which the web UI's PIN gate never relies on — see
  // WebUiServer.h) — callers always supply a generated password long
  // enough that this can't silently fail on length.
  return WiFi.softAP(ssid, password);
}

void WifiManager::stopAccessPoint() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace net
