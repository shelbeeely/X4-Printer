// Focusink firmware for the Xteink X4 e-paper device — entry point.
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

#include "calendar/WakeSchedule.h"
#include "config/AppSettings.h"
#include "config/CalendarCache.h"
#include "config/CalendarConfig.h"
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
// Timeline screen's user-authored tasks (ui/PlannerUI.h, docs/planner.md).
// Populated by a Pi sync pass in a future unit; loaded/saved here the same
// way jobIndex/outboxIndex are so the SD-backed store round-trips
// correctly even before that sync glue exists.
store::TaskIndex plannerTaskIndex;
config::DeviceConfigData deviceConfig;
config::AppSettingsData appSettings;
config::NextEventInfo nextEvent;

uint32_t lastActivityMs = 0;

void runSyncPass() {
  syncmgr::SyncManager manager(deviceConfig, jobIndex, outboxIndex);
  uiState.lastSyncSummary = manager.runFullSync();
  uiState.hasSyncedOnce = true;
  // SyncManager::runFullSync() writes a fresh calendar::syncCalendars()
  // result straight to config::CalendarCache's singleton (see
  // sync/SyncManager.cpp) -- re-read it into main.cpp's copy so the
  // Inbox screen picks up this wake's sync immediately rather than only
  // after the next reboot, same reason deviceConfig/appSettings are
  // local copies of their own singletons instead of reaching the
  // singleton directly from ui/InboxUI.cpp.
  nextEvent = config::CalendarCache::instance().data();
}

// `intervalSeconds` is the caller's already-computed effective sleep
// duration -- see nextWakeIntervalSeconds() below -- not always
// kWakeTimerIntervalSeconds: a nearer calendar wake target (Settings >
// Calendar tab; calendar/WakeSchedule.h) shortens it.
void goToSleep(uint32_t intervalSeconds) {
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
  store::savePlannerIndex(plannerTaskIndex);
  power::SleepManager::sleepUntilNextEvent(intervalSeconds);  // noreturn
}

// How long to sleep until the next thing that needs this device awake:
// the regular hourly background sync, or a sooner calendar wake target
// that hasn't fired yet (Settings > Calendar tab). Used for every sleep
// that ISN'T itself the moment a calendar reminder fires (that path
// already has its own freshly computed decision — see setup()'s Timer
// branch) — the idle-timeout sleep in loop(), and a Timer wake with
// nothing due right now. Deliberately computes against a scratch copy of
// nextEvent: this is a read-only "how long" query, never a place that
// should mark a reminder as fired (that only happens where the frame
// actually gets rendered).
uint32_t nextWakeIntervalSeconds() {
  config::NextEventInfo scratch = nextEvent;
  calendar::WakeDecision decision =
      calendar::computeWakeDecision(appSettings, scratch, time(nullptr), kWakeTimerIntervalSeconds);
  return decision.sleepSeconds;
}

// Constructs the FreeInkUI App exactly once per boot (the underlying
// DisplayTarget/App are `static` locals, which can't be declared twice in
// one function) and runs initApp() the first time. Shared by the normal
// interactive wake path and the calendar-reminder path below — both need
// a real App to render a frame, but only one of them runs per boot.
ui::App& ensureApp() {
  static freeink::ui::DisplayTarget displayTarget(display.getFrameBuffer(), display.getDisplayWidth(),
                                                  display.getDisplayHeight(), display.getDisplayWidthBytes());
  static ui::App appInstance(displayTarget, displayTarget.deviceContext());
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    // Deliberately no appInstance.setClearColor(...) — the reader screen
    // relies on frames NOT being auto-cleared so its raw framebuffer page
    // write persists under the footer chrome drawn on top of it. See
    // ui/InboxUI.h's header comment.
    ui::initApp(appInstance, uiState);
  }
  return appInstance;
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
  // Captured first, before anything else runs: the web UI's diagnostics
  // route reports "uptime since this wake" as millis() - wakeMillis, which
  // only means anything if it's measured from as close to the actual wake
  // as this code can get. Deep sleep has no way to persist a monotonic
  // clock across sleep cycles without RTC-memory plumbing this doesn't
  // need — this is uptime since wake, not device lifetime uptime.
  uiState.wakeMillis = millis();

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
  config::AppSettings::instance().load();
  appSettings = config::AppSettings::instance().data();
  config::CalendarConfig::instance().load();
  config::CalendarCache::instance().load();
  nextEvent = config::CalendarCache::instance().data();

  store::loadJobIndex(jobIndex);
  store::loadApprovalOutbox(outboxIndex);
  store::loadPlannerIndex(plannerTaskIndex);

  uiState.jobs = &jobIndex;
  uiState.outbox = &outboxIndex;
  uiState.plannerTasks = &plannerTaskIndex;
  uiState.deviceConfig = &deviceConfig;
  uiState.appSettings = &appSettings;
  uiState.nextEvent = &nextEvent;
  uiState.framebuffer = display.getFrameBuffer();
  uiState.framebufferSize = display.getBufferSize();
  uiState.panelWidth = display.getDisplayWidth();
  uiState.panelHeight = display.getDisplayHeight();

  if (wakeReason == power::WakeReason::Timer) {
    // Nobody is looking at the screen for a plain timer wake: sync
    // silently. This is the "use deep sleep aggressively... wake from a
    // timer, synchronize, then return to sleep" path from the task
    // description — UNLESS a calendar wake target (Settings > Calendar
    // tab) is due, in which case we render exactly one reminder frame
    // before sleeping again (see calendar/WakeSchedule.h).
    runSyncPass();

    calendar::WakeDecision decision =
        calendar::computeWakeDecision(appSettings, nextEvent, time(nullptr), kWakeTimerIntervalSeconds);
    if (decision.alert == calendar::WakeAlertKind::None) {
      goToSleep(decision.sleepSeconds);  // noreturn
    }

    // A reminder is due: computeWakeDecision() already stamped the fired
    // dedup marker into `nextEvent` above — persist it right away so a
    // crash or power loss between here and the next sync can't re-fire
    // the same target, then render the one frame and go straight back to
    // sleep. The e-paper panel holds that image with no power until the
    // next wake redraws it, so there's nothing to wait on here.
    config::CalendarCache::instance().set(nextEvent);
    config::CalendarCache::instance().save();

    uiState.calendarReminderKind = decision.alert == calendar::WakeAlertKind::BeforeStart
                                        ? ui::CalendarReminderKind::BeforeStart
                                        : ui::CalendarReminderKind::AtEnd;
    uiState.mode = ui::ScreenMode::CalendarReminder;

    app = &ensureApp();
    freeink::ui::InputSnapshot noInput{};
    freeink::ui::ActionEvent event = app->render(noInput);
    (void)event;
    freeink::ui::present(display, app->lastRenderRefreshHint());

    goToSleep(nextWakeIntervalSeconds());  // noreturn
  }

  app = &ensureApp();

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
    goToSleep(nextWakeIntervalSeconds());  // noreturn
  }
}
