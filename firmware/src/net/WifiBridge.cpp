#include "net/WifiBridge.h"

#include <Arduino.h>
#include <WiFi.h>

// lwip_napt.h's own API is only compiled into lwIP when IP_NAPT (from
// CONFIG_LWIP_IPV4_NAPT) is set — the same guard esp32_nat_router's
// main/esp32_nat_router.c uses, so a misconfigured custom_sdkconfig
// (firmware/platformio.ini) fails the build here instead of silently
// leaving Hotspot mode without passthrough.
#include "lwip/lwip_napt.h"
#if !IP_NAPT
#error "IP_NAPT must be enabled (CONFIG_LWIP_IP_FORWARD + CONFIG_LWIP_IPV4_NAPT in firmware/platformio.ini's custom_sdkconfig)"
#endif

namespace net {

bool WifiBridge::start(const char* apSsid, const char* apPassword, uint32_t staTimeoutMs) {
  hasUplink_ = false;

  // WIFI_AP_STA up front: joinKnownNetwork() below must not disturb the
  // softAP that's about to start, so it's never handed WIFI_STA the way
  // WifiManager::connect() sets it for the plain-Station-mode case.
  WiFi.mode(WIFI_AP_STA);
  // softAP() requires an 8+ character password (or empty for an open
  // network, which the web UI's PIN gate never relies on — see
  // ui/WebUiServer.h) — callers always supply a generated password long
  // enough that this can't silently fail on length.
  if (!WiFi.softAP(apSsid, apPassword)) {
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // Best-effort: no saved network in range just means no passthrough,
  // never a failed hotspot (see WifiBridge.h).
  if (wifi_.joinKnownNetwork(staTimeoutMs)) {
    ip_napt_enable(static_cast<uint32_t>(WiFi.softAPIP()), 1);
    hasUplink_ = true;
  }
  return true;
}

void WifiBridge::stop() {
  // Tearing down the radio (WIFI_OFF) drops both interfaces; there's no
  // matching ip_napt_disable() in lwIP, but with the netifs gone there's
  // nothing left for NAPT to forward for.
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  hasUplink_ = false;
}

}  // namespace net
