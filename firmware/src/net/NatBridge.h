#pragma once
// Optional, opt-in dual AP+STA NAT bridging for the on-device Web UI's
// hotspot mode (ui/WebUiServer.h) -- lets a phone joined to the X4's own
// SoftAP also reach the Pi/internet, instead of staying fully isolated
// the way hotspot mode works today. See docs/architecture.md "On-device
// Web UI (opt-in)" and docs/security.md for the tradeoff writeup: this is
// a further, still-opt-in broadening of the existing "X4 never accepts
// inbound connections" exception, gated behind
// config::AppSettingsData::hotspotNatBridgeEnabled (default off) on top
// of the Web UI's own opt-in toggle.
//
// Feasibility, verified directly against this project's actual pinned
// toolchain (not assumed from general ESP32/lwIP documentation): the
// pioarduino platform-espressif32 build this project's platformio.ini
// pins (Arduino-ESP32 core 3.3.7 / ESP-IDF 5.5) ships a precompiled
// liblwip.a with CONFIG_LWIP_IP_FORWARD=y and CONFIG_LWIP_IPV4_NAPT=y --
// confirmed both via that build's sdkconfig and via `nm` on the actual
// .a file, which exports ip_napt_enable/ip_napt_enable_netif/
// ip_napt_forward/ip_napt_recv. Real transparent NAT (the same
// lwip/lwip_napt.h API esp32_nat_router itself uses) is genuinely
// available here, not just a scoped single-port forward.

#include <cstdint>

namespace net {

class NatBridge {
 public:
  // Brings up a concurrent STA connection to a known saved network
  // (net::WifiManager::connectKeepingAp() -- preserves the AP that must
  // already be running, unlike plain connect()) and enables lwIP NAPT on
  // the resulting STA interface IP, so traffic from AP-side (hotspot)
  // clients gets translated and forwarded out through it. Returns false
  // (bridge not enabled, AP itself untouched either way) if no known
  // network is in range/reachable within timeoutMs.
  bool enable(uint32_t timeoutMs = 15000);

  // Disables NAPT on whichever IP enable() last enabled it on (a no-op if
  // never enabled) and disconnects the STA link, leaving the AP running
  // isolated again -- today's default hotspot behavior.
  void disable();

  bool isEnabled() const { return enabled_; }

 private:
  bool enabled_ = false;
  uint32_t bridgedStaIp_ = 0;  // the IP NAPT was last enabled on, for disable()
};

}  // namespace net
