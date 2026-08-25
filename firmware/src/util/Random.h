#pragma once
// Shared ESP32-hardware-RNG-backed random hex string generator. Used
// anywhere this firmware needs an unguessable/unique-enough id —
// approval ids (ui/InboxUI.cpp, ui/WebUiServer.cpp) and the web UI's
// session token (ui/WebUiServer.cpp) — from one place, rather than each
// call site reimplementing the same loop (which is exactly the kind of
// small drift-prone duplication that's easy to fix in one copy and
// silently leave stale in the other).
//
// esp_random() is the ESP32's true-entropy hardware RNG, not a PRNG
// seeded from millis() — good enough here because an approval id only
// needs to never collide with another id this device generates (the
// server dedups per approval_id globally, so a collision would just look
// like a harmless retried duplicate, not a wrong action), and a session
// token only needs to be unguessable within one toggle-on session's
// lifetime.

#include <cstddef>

#include <esp_system.h>

namespace fwrand {

// Writes exactly n lowercase hex characters + a NUL terminator into out,
// which must be at least n+1 bytes.
inline void randomHex(char* out, size_t n) {
  static const char* kHex = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) out[i] = kHex[esp_random() & 0xF];
  out[n] = '\0';
}

}  // namespace fwrand
