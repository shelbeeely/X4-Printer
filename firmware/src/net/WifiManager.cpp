#include "net/WifiManager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config/WifiStore.h"

namespace net {

bool WifiManager::connect(uint32_t timeoutMs) {
  config::WifiStore& store = config::WifiStore::instance();
  if (store.count() == 0) {
    return false;  // nothing saved yet — normal on a freshly-provisioned device
  }

  WiFi.mode(WIFI_STA);
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
      WiFi.disconnect(true);
      return false;
    }
    delay(100);
  }

  store.setLastConnectedSsid(best->ssid);
  store.save();
  return true;
}

void WifiManager::disconnect() {
  // wifioff=true, eraseap=true: also clears the ESP32 WiFi driver's own NVS
  // AP record. Safe — this firmware never relies on the driver's built-in
  // auto-reconnect; connect() always re-supplies credentials from
  // config::WifiStore explicitly on every wake.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

bool WifiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }

String WifiManager::currentSsid() const { return WiFi.SSID(); }

String WifiManager::currentIp() const { return WiFi.localIP().toString(); }

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
