#pragma once
// Print Inbox UI: built on FreeInkUI's FreeInkApp/Screen builder
// (freeink-sdk/libs/ui/FreeInkUI, see docs/freeink-ui.md) exactly the way
// its own docs show — one FreeInkApp instance, screen functions that build
// UI each render pass, semantic actions routed through app.on(...).
//
// The one deliberate departure from the textbook FreeInkUI flow is the
// reader screen: FreeInkDisplay has no text/shape drawing primitives of
// its own (only clearScreen/drawImage/displayBuffer — see
// freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h), and a
// downloaded page is a raw 1bpp bitmap already sized to the panel, so
// XtcReader writes it directly into the same framebuffer memory
// FreeInkUI's DisplayTarget wraps — no intermediate bitmap buffer, no
// DrawTarget::bitmap() call, consistent with docs/xtc-format.md's "stream
// straight into the framebuffer" design. FreeInkApp does not auto-clear the
// framebuffer between renders unless setClearColor() is set (see
// docs/freeink-ui.md's "Frames do not clear the target on their own"), so
// this firmware deliberately never calls setClearColor(): the reader
// screen's raw page write IS the frame's background, and screen.footer()
// draws its chrome band on top of it in the same pass.

#include <cstdint>

#include <FreeInkApp.h>

#include "config/AppSettings.h"
#include "config/CalendarCache.h"
#include "config/DeviceConfig.h"
#include "store/ApprovalOutbox.h"
#include "store/JobStore.h"
#include "store/PlannerStore.h"
#include "sync/SyncManager.h"
#include "ui/WebUiServer.h"
#include "xtc/XtcReader.h"

namespace freeink_display {
class FreeInkDisplay;  // forward decl to avoid pulling the full header here
}

namespace ui {

using App = freeink::ui::FreeInkApp<48, 24>;  // interaction/handler capacities — see docs/freeink-ui.md

enum class ScreenMode {
  Inbox,     // list of downloaded documents
  Reader,    // paging through the selected document
  ActionMenu,  // Print / Keep / Delete / Cancel for the selected document
  Status,    // last sync result / pairing status
  WebUiChoice,  // "Use Wi-Fi" / "Use Hotspot" / "Cancel" — entry point for the on-device web UI
  WebUi,        // web UI is running: shows connection info + PIN + Stop
  Settings,     // tabbed settings — see SettingsTab
  Timeline,     // day view merging store::PlannerStore tasks + the calendar
                // module's next event — see ui/PlannerUI.h, docs/planner.md
  // Rendered exactly once on a Timer wake that lands within a configured
  // calendar-reminder window (Settings > Calendar tab; see
  // calendar/WakeSchedule.h), then main.cpp sleeps immediately — nobody is
  // expected to be holding the device when a background timer wake fires,
  // so there's no interactive follow-up screen for this the way the other
  // modes have.
  CalendarReminder,
};

// Which threshold fired the reminder currently showing (ScreenMode::
// CalendarReminder) -- set by main.cpp right before it builds/renders the
// UI for that one frame.
enum class CalendarReminderKind : uint8_t {
  BeforeStart,
  AtEnd,
};

// One tab per settings group, matching the "tabbed groups" convention the
// FreeInk SDK's other apps use for on-device settings (WakeInk's
// Alarm/Sound/Filter/Clock/System tabs, sticky-reminders' Wi-Fi & clock
// group) — switched with the Prev/Next footer actions, not a touch tab bar
// the X4 has no hardware for.
enum class SettingsTab : uint8_t {
  Wifi = 0,       // saved networks: view + remove (adding a network needs
                   // text entry this device has no keyboard for — done from
                   // the on-device Web UI instead, see WebUiServer.h)
  SyncRelay = 1,  // pairing/relay info -- read-only, provisioned by
                  // pi-server/tools/pair_device.py, not editable on-device
  Display = 2,    // default reading view (portrait/landscape)
  Calendar = 3,   // wake-before-event / wake-at-event-end reminders -- see
                  // calendar/WakeSchedule.h
  DeviceInfo = 4,  // firmware version, battery, storage, uptime -- read-only
  Planner = 5,     // Timeline orientation (vertical/horizontal) -- see
                    // ui/PlannerUI.h. Appended after DeviceInfo rather than
                    // inserted earlier, so existing tab positions/muscle
                    // memory don't shift.
  kCount = 6,
};

struct InboxUiState {
  store::JobIndex* jobs = nullptr;
  store::ApprovalOutboxIndex* outbox = nullptr;
  // Timeline screen's user-authored tasks (ui/PlannerUI.h,
  // docs/planner.md) -- loaded/saved by main.cpp the same way jobs/outbox
  // are. Merged with nextEvent below at render time, not stored merged.
  store::TaskIndex* plannerTasks = nullptr;
  const config::DeviceConfigData* deviceConfig = nullptr;
  // Not const: the Settings screen's Display tab writes through this
  // (config::AppSettings::instance().data(), wired by main.cpp) and saves
  // via config::AppSettings::instance().save() on change.
  config::AppSettingsData* appSettings = nullptr;
  // Refreshed by main.cpp's runSyncPass() after every sync pass (see that
  // function's comment) -- the Inbox screen's idle state reads this when
  // there are no print jobs to review. Read-only from the UI's side.
  const config::NextEventInfo* nextEvent = nullptr;

  // On-device web UI (ui/WebUiServer.h) — see docs/architecture.md
  // "On-device Web UI". A plain member (not a pointer): its own
  // jobs/outbox/deviceConfig pointers are wired via attach() in
  // initApp(), the same post-construction pattern the fields above use.
  WebUiServer webUiServer;
  // Set inside the ActionWebUiChoiceRowSelect handler (InboxUI.cpp) on a
  // failed startStation()/startHotspot(); webUiChoiceScreen() shows it
  // once, then clears it.
  const char* webUiStartError = nullptr;

  ScreenMode mode = ScreenMode::Inbox;
  int16_t selectedJobIndex = -1;
  uint16_t currentPage = 0;

  xtc::XtcReader reader;
  bool readerOpenForSelected = false;
  // True while showing the landscape-strip variant (docs/protocol.md §4)
  // instead of the normal portrait-fit one. Reset to false whenever a new
  // document is opened (ActionOpenJob) -- every fresh open defaults to the
  // normal view.
  bool landscapeView = false;

  syncmgr::SyncSummary lastSyncSummary;
  bool hasSyncedOnce = false;

  // Settings screen (ScreenMode::Settings) navigation state -- which tab is
  // showing and which Wi-Fi network row (if any) is pending a remove
  // confirmation. Persists across renders the same way selectedJobIndex
  // does, so re-entering Settings picks up where the user left it within
  // one wake window (not saved anywhere; resets to Wifi/-1 next boot).
  SettingsTab settingsTab = SettingsTab::Wifi;
  int16_t settingsWifiRemoveIndex = -1;

  // Set by main.cpp right before it renders the one-shot
  // ScreenMode::CalendarReminder frame -- see that enum's comment.
  CalendarReminderKind calendarReminderKind = CalendarReminderKind::BeforeStart;

  // Set by main.cpp before each app.render() call so screen functions can
  // reach the raw framebuffer for the reader's direct page write.
  uint8_t* framebuffer = nullptr;
  size_t framebufferSize = 0;
  uint16_t panelWidth = 0;
  uint16_t panelHeight = 0;
  // Set by main.cpp's setup() as its very first statement, before anything
  // else runs — millis() at the top of this wake. The web UI's diagnostics
  // route reports uptime-since-wake as millis() - wakeMillis; this resets
  // to 0 every deep-sleep cycle (not a device-lifetime uptime).
  uint32_t wakeMillis = 0;

  // Signals to main.cpp's loop, set by action handlers, cleared after
  // being acted on. Keeps SyncManager/SleepManager out of the UI layer.
  bool requestSyncNow = false;
  bool requestSleep = false;
};

// Builds the App instance and registers every action handler. Call once in
// setup() after the display/UI DisplayTarget are constructed.
void initApp(App& app, InboxUiState& state);

}  // namespace ui
