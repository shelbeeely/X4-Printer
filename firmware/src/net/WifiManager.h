#pragma once
// Wi-Fi connect/disconnect for the sync window. Deliberately minimal: scan,
// pick the strongest visible network that matches a saved credential,
// connect with a bounded timeout, and — critically for "use deep sleep
// aggressively" — a hard disconnect()+radio-off when the sync window ends,
// never a background/idle Wi-Fi state. See docs/architecture.md "Deep
// sleep / wake sequence" steps 2 and 8.

#include <cstdint>

#include <WString.h>

namespace net {

class WifiManager {
 public:
  // Scans for networks, matches against config::WifiStore's saved
  // credentials, and connects to the strongest match. Returns false (not
  // an error — a normal state, see docs/architecture.md step 2) if no
  // saved network is visible or the connection doesn't complete within
  // timeoutMs.
  bool connect(uint32_t timeoutMs = 15000);

  // Tears down the radio. Always safe to call even if connect() was never
  // called or already failed.
  void disconnect();

  bool isConnected() const;
  String currentSsid() const;
  String currentIp() const;  // station-mode IP, e.g. for the web UI's status screen

  // Live RSSI (dBm, negative — closer to 0 is stronger) of the currently
  // associated AP. Only meaningful when isConnected(); this class holds no
  // persistent state (a fresh WifiManager is constructed per call site, see
  // ui/WebUiServer.cpp), so like isConnected()/currentSsid()/currentIp()
  // above, this reads the radio's live state directly rather than
  // remembering a value from connect()'s own internal network scan.
  int32_t rssi() const;

  // Broadcasts this device's own hotspot (WiFi.softAP) — for the on-device
  // web UI's "mobile, no known network in range" mode (ui/WebUiServer.h).
  // Unlike connect(), this deliberately does NOT consult WifiStore: it's
  // an explicit, user-toggled alternative to joining a known network, not
  // part of the normal sync-window Wi-Fi flow. Default AP gateway
  // (192.168.4.1) — no custom softAPConfig, same "deliberately minimal"
  // style as connect()/disconnect() above.
  bool startAccessPoint(const char* ssid, const char* password);
  void stopAccessPoint();
};

}  // namespace net
