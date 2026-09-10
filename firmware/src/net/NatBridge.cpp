#include "net/NatBridge.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/lwip_napt.h>

#include "net/WifiManager.h"

namespace net {

bool NatBridge::enable(uint32_t timeoutMs) {
  if (enabled_) disable();  // defensive: re-enable() while already on re-does the bridge cleanly

  WifiManager wifi;
  if (!wifi.connectKeepingAp(timeoutMs)) {
    return false;  // no known network in range/reachable -- AP itself is untouched
  }

  bridgedStaIp_ = static_cast<uint32_t>(WiFi.localIP());
  ip_napt_enable(bridgedStaIp_, 1);
  enabled_ = true;
  return true;
}

void NatBridge::disable() {
  if (!enabled_) return;
  ip_napt_enable(bridgedStaIp_, 0);
  bridgedStaIp_ = 0;
  enabled_ = false;

  // Drop the STA link only, leaving the softAP (and whichever
  // WebUiServer session is using it) running untouched -- see
  // WifiManager::disconnectKeepingAp(), the connectKeepingAp()
  // counterpart this mirrors.
  WifiManager().disconnectKeepingAp();
}

}  // namespace net
