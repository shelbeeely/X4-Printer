#include "power/SleepManager.h"

#include <PowerManager.h>
#include <esp_sleep.h>
#include <esp_system.h>

namespace power {

WakeReason SleepManager::determineWakeReason() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_TIMER) return WakeReason::Timer;
  // RISC-V (ESP32-C3, the X4's MCU) wakes via the "gpio" source; Xtensa
  // parts via ext1 — freeink::PowerManager::armPowerButtonWakeup() picks
  // the SoC-correct one at arm time (see PowerManager.h), so checking both
  // possible causes here keeps this code correct if this firmware is ever
  // built for another FreeInk-supported board.
  if (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1) return WakeReason::PowerButton;

  esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_USB) return WakeReason::AfterUsbOrFlash;
  return WakeReason::Unknown;
}

void SleepManager::sleepUntilNextEvent(uint32_t timerIntervalSeconds) {
  freeink::PowerManager::waitForPowerButtonRelease();
  freeink::PowerManager::armPowerButtonWakeup();

  if (timerIntervalSeconds > 0) {
    esp_sleep_enable_timer_wakeup(uint64_t(timerIntervalSeconds) * 1000000ULL);
  }

  // Cuts power to any peripheral rails this board gates (no-op on X4,
  // whose display/SD rails are PIN_UNASSIGNED per PowerManager.h) and
  // isolates floating GPIOs before the actual sleep entry.
  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleep();  // noreturn
}

}  // namespace power
