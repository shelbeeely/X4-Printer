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
};

}  // namespace net
