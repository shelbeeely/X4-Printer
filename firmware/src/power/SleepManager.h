#pragma once
// Deep-sleep orchestration. Thin wrapper over freeink::PowerManager +
// InputManager (the SDK owns the actual per-SoC deep-sleep mechanics — see
// docs/architecture.md's FreeInk SDK paragraph); this module only decides
// *when* and records *why* the device woke, for SyncManager/InboxUI to act
// on (a timer wake syncs and goes right back to sleep; a button wake opens
// the inbox UI and stays awake until user idle timeout).

#include <cstdint>

namespace power {

enum class WakeReason {
  PowerButton,
  Timer,
  AfterUsbOrFlash,
  Unknown,
};

class SleepManager {
 public:
  // Call once early in setup(), before any FreeInk display/SD init that
  // depends on knowing why the device woke.
  static WakeReason determineWakeReason();

  // Arms both the power button and (if intervalSeconds > 0) an RTC timer
  // wake, then enters deep sleep. Never returns — the next code to run is
  // setup() after the chip resets.
  [[noreturn]] static void sleepUntilNextEvent(uint32_t timerIntervalSeconds);
};

}  // namespace power
