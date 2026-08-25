// Xteink X4 Print Inbox firmware — entry point.
//
// Implements docs/architecture.md's wake sequence end to end: determine
// why we woke (power button vs RTC timer), sync (Wi-Fi connect, download
// pending jobs, drain the approval outbox), then either go straight back
// to sleep (timer wake — no one is looking at the screen) or open the
// Print Inbox UI and stay awake until the user is idle, then sleep.
//
// Built entirely on FreeInk SDK components (EInkDisplay, SDCardManager,
// InputManager, PowerManager, FreeInkUI/FreeInkApp) — see
// docs/architecture.md for what's reused from each reference project.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>
#include <PowerManager.h>
#include <SDCardManager.h>

#include "config/DeviceConfig.h"
#include "config/WifiStore.h"
#include "power/SleepManager.h"
#include "store/ApprovalOutbox.h"
#include "store/JobStore.h"
#include "sync/SyncManager.h"
#include "ui/InboxUI.h"

// FreeInkUI's render pipeline (screen builders + text layout) runs deeper
// than Arduino's default 8KB loopTask stack; this firmware additionally
// nests HTTPClient/WiFiClientSecure/mbedTLS TLS handshakes and ArduinoJson
// parsing under the same call stack during a sync pass, so the SDK's own
// 16KB weak default is not enough headroom — see docs/freeink-ui.md.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

namespace {

constexpr uint32_t kIdleSleepTimeoutMs = 90000;        // no input for 90s -> sleep
constexpr uint32_t kWakeTimerIntervalSeconds = 3600;   // hourly background sync

EInkDisplay display(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
                    BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
                    BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
InputManager buttons;

ui::InboxUiState uiState;
ui::App* app = nullptr;

store::JobIndex jobIndex;
store::ApprovalOutboxIndex outboxIndex;
config::DeviceConfigData deviceConfig;

uint32_t lastActivityMs = 0;

void runSyncPass() {
  sync::SyncManager manager(deviceConfig, jobIndex, outboxIndex);
  uiState.lastSyncSummary = manager.runFullSync();
  uiState.hasSyncedOnce = true;
}

void goToSleep() {
  // Nothing else tears down an on-device web UI session (started outside
  // the normal sync flow, see ui/WebUiServer.h) before deep sleep —
  // without this, an AP/STA radio left up by that feature would still be
  // "on" across the sleep transition. Safe no-op if it was never started.
  if (uiState.webUiServer.isActive()) {
    uiState.webUiServer.stop();
  }

  // Persist one more time before sleeping, defensively — SyncManager and
  // the UI action handlers already save after every mutation, so this is
  // normally a no-op write, not a load-bearing final flush.
  store::saveJobIndex(jobIndex);
  store::saveApprovalOutbox(outboxIndex);
  power::SleepManager::sleepUntilNextEvent(kWakeTimerIntervalSeconds);  // noreturn
}

freeink::ui::InputSnapshot readInputSnapshot() {
  freeink::ui::InputSnapshot input{};
  input.confirm = buttons.wasPressed(InputManager::BTN_CONFIRM);
  input.back = buttons.wasPressed(InputManager::BTN_BACK);
  input.focusNext = buttons.wasPressed(InputManager::BTN_DOWN) || buttons.wasPressed(InputManager::BTN_RIGHT);
  input.focusPrev = buttons.wasPressed(InputManager::BTN_UP) || buttons.wasPressed(InputManager::BTN_LEFT);
  return input;
}

}  // namespace

void setup() {
#ifdef ENABLE_SERIAL_LOG
  Serial.begin(115200);
#endif

  power::WakeReason wakeReason = power::SleepManager::determineWakeReason();

  SdMan.begin();
  buttons.begin();
  display.begin();

  config::DeviceConfig::instance().load();
  deviceConfig = config::DeviceConfig::instance().data();
  config::WifiStore::instance().load();

  store::loadJobIndex(jobIndex);
  store::loadApprovalOutbox(outboxIndex);

  uiState.jobs = &jobIndex;
  uiState.outbox = &outboxIndex;
  uiState.deviceConfig = &deviceConfig;
  uiState.framebuffer = display.getFrameBuffer();
  uiState.framebufferSize = display.getBufferSize();
  uiState.panelWidth = display.getDisplayWidth();
  uiState.panelHeight = display.getDisplayHeight();

  if (wakeReason == power::WakeReason::Timer) {
    // Nobody is looking at the screen for a timer wake: sync silently and
    // go straight back to sleep without ever building a UI frame. This is
    // the "use deep sleep aggressively... wake from a timer, synchronize,
    // then return to sleep" path from the task description.
    runSyncPass();
    goToSleep();  // noreturn
  }

  static freeink::ui::DisplayTarget displayTarget(display.getFrameBuffer(), display.getDisplayWidth(),
                                                  display.getDisplayHeight(), display.getDisplayWidthBytes());
  static ui::App appInstance(displayTarget, displayTarget.deviceContext());
  app = &appInstance;
  // Deliberately no app->setClearColor(...) — the reader screen relies on
  // frames NOT being auto-cleared so its raw framebuffer page write
  // persists under the footer chrome drawn on top of it. See
  // ui/InboxUI.h's header comment.
  ui::initApp(*app, uiState);

  // Power-button / USB / cold-boot wake: sync once at boot (bounded by
  // SyncClient's own HTTP timeouts, never blocks indefinitely) so the
  // inbox is current the moment the UI appears, then open it.
  runSyncPass();

  lastActivityMs = millis();
}

void loop() {
  buttons.update();
  bool anyInput = buttons.wasAnyPressed();

  freeink::ui::InputSnapshot input = readInputSnapshot();
  freeink::ui::ActionEvent event = app->render(input);
  (void)event;
  freeink::ui::present(display, app->lastRenderRefreshHint());

  if (anyInput) {
    lastActivityMs = millis();
  }

  if (uiState.requestSyncNow) {
    uiState.requestSyncNow = false;
    runSyncPass();
    app->invalidate(freeink::ui::RefreshHint::Full);
    lastActivityMs = millis();
  }

  // On-device web UI (ui/WebUiServer.h): service at most the currently
  // pending request each pass, and treat a phone actually using the page
  // (its periodic /api/status poll) as activity — same idle timer as
  // physical button presses, no separate timeout for this mode.
  if (uiState.mode == ui::ScreenMode::WebUi && uiState.webUiServer.isActive()) {
    uiState.webUiServer.handleClient();
    if (uiState.webUiServer.consumeActivityFlag()) {
      lastActivityMs = millis();
    }
  }

  if (millis() - lastActivityMs > kIdleSleepTimeoutMs) {
    goToSleep();  // noreturn
  }
}
