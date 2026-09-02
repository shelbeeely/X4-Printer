#pragma once
// Concurrent AP+STA "internet passthrough" for the on-device web UI's
// Hotspot mode (ui/WebUiServer.cpp's startHotspot()): broadcasts this
// device's own AP *and* joins a saved network as a station at the same
// time, then NATs the AP subnet out through the station link — so a
// phone that connects to the hotspot to browse the print queue keeps its
// own internet access instead of losing it for the session.
//
// Reuses the lwIP IP_NAPT mechanism the way
// github.com/martin-ger/esp32_nat_router does: CONFIG_LWIP_IP_FORWARD +
// CONFIG_LWIP_IPV4_NAPT (enabled for this firmware via
// firmware/platformio.ini's custom_sdkconfig) plus a single
// ip_napt_enable() call once both interfaces are up. See
// docs/architecture.md's "Hotspot internet passthrough" section for the
// attribution and exactly what is/isn't reused.
//
// Deliberately best-effort and scoped to Hotspot mode only: if no saved
// network is in range, the hotspot still comes up standalone (same as
// before WifiBridge existed) with no passthrough — this never blocks or
// delays Hotspot mode, and Station mode (WebUiServer::startStation) is
// untouched. This is an extension of the same documented, scoped
// exception to "X4 never accepts inbound connections" that Hotspot mode
// already is (see CLAUDE.md's invariants and docs/security.md) — it does
// not add any new listening surface on the X4 itself, only forwards a
// connected phone's own traffic out to the internet.

#include <cstdint>

#include "net/WifiManager.h"

namespace net {

class WifiBridge {
 public:
  // Starts the hotspot (WiFi.softAP, default AP gateway 192.168.4.1 — no
  // custom softAPConfig, same "deliberately minimal" style as
  // WifiManager::connect()/disconnect()) and, if a saved network is
  // visible within staTimeoutMs, also joins it as a station and enables
  // NAT from the AP subnet out through it. Returns false only if the
  // hotspot itself fails to start; check hasUplink() to see whether
  // passthrough actually came up.
  bool start(const char* apSsid, const char* apPassword, uint32_t staTimeoutMs = 8000);

  // Tears down both interfaces and the radio. Safe to call even if
  // start() only got as far as the hotspot (no uplink joined).
  void stop();

  // True once a saved network was actually joined and NAT was enabled
  // for the current session — for the web UI status screen
  // (ui/InboxUI.cpp's webUiScreen()).
  bool hasUplink() const { return hasUplink_; }

 private:
  WifiManager wifi_;
  bool hasUplink_ = false;
};

}  // namespace net
