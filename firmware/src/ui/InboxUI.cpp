#include "ui/InboxUI.h"

#include <ArduinoJson.h>  // pulled transitively by other headers; kept explicit for clarity
#include <BatteryMonitor.h>
#include <SDCardManager.h>
#include <time.h>

#include <cstdio>
#include <cstring>

#include "config/Version.h"
#include "config/WifiStore.h"
#include "store/AtomicJsonFile.h"
#include "util/Random.h"

namespace ui {

namespace {

enum : freeink::ui::ActionId {
  ActionOpenJob = 1,
  ActionBack = 2,
  ActionNextPage = 3,
  ActionPrevPage = 4,
  ActionShowActionMenu = 5,
  ActionCancelMenu = 6,
  ActionSyncNow = 7,
  ActionMenuRowSelect = 8,
  ActionOpenWebUiChoice = 9,
  ActionWebUiChoiceRowSelect = 10,
  ActionWebUiStop = 11,
  ActionCancelWebUiChoice = 12,
  ActionOpenSettings = 13,
  ActionSettingsBack = 14,
  ActionSettingsPrevTab = 15,
  ActionSettingsNextTab = 16,
  ActionSettingsWifiRowSelect = 17,
  ActionSettingsWifiConfirmRemove = 18,
  ActionSettingsWifiCancelRemove = 19,
  ActionSettingsDisplayRowSelect = 20,
  // One action id for all three Calendar tab rows -- event.value picks the
  // row, same "list fires one action, the handler reads event.value"
  // pattern actionMenuScreen/ActionMenuRowSelect uses.
  ActionSettingsCalendarRowSelect = 21,
};

// Cycled through on select by the Calendar settings tab's lead-time row --
// a fixed preset list rather than a free-entry stepper, same reasoning as
// the Wi-Fi tab's view/remove-only scope: this device has no keyboard/dial
// to type or scroll an arbitrary number with.
constexpr uint16_t kCalendarLeadMinutePresets[] = {5, 10, 15, 30, 60};
constexpr size_t kCalendarLeadMinutePresetCount =
    sizeof(kCalendarLeadMinutePresets) / sizeof(kCalendarLeadMinutePresets[0]);

const char* statusGlyph(store::JobStatus status) {
  switch (status) {
    case store::JobStatus::Downloaded:
      return "*";  // unread
    case store::JobStatus::ApprovedPrint:
      return "P";
    case store::JobStatus::ApprovedKeep:
      return "K";
    case store::JobStatus::ApprovedDelete:
      return "D";
  }
  return " ";
}

// Section ordering for the Inbox list -- New (unreviewed) first since
// that's the reason the device woke up, then the two "already decided,
// still on SD pending sync" states so the user can see what's queued
// without hunting for it in one flat, unordered list. ApprovedDelete stays
// hidden entirely (unchanged from before this grouping): those jobs still
// exist on disk pending sync/retention (docs/architecture.md's data
// model), but there's nothing left for the user to do with them.
struct JobSection {
  const char* header;
  store::JobStatus status;
};
constexpr JobSection kJobSections[] = {
    {"New", store::JobStatus::Downloaded},
    {"To Print", store::JobStatus::ApprovedPrint},
    {"Kept", store::JobStatus::ApprovedKeep},
};
constexpr size_t kJobSectionCount = sizeof(kJobSections) / sizeof(kJobSections[0]);

// items[]/labelBuf[] must have room for kMaxInboxJobs real rows PLUS
// kJobSectionCount header rows (see the caller's array sizing) -- maxJobs
// caps only the real-job count (kMaxInboxJobs), independent of that larger
// physical capacity, so the header insertion below always has room.
//
// Builds the list rows shown on the Inbox screen, grouped into section
// headers by status (see kJobSections) -- FreeInkUI's list component skips
// header rows for focus/selection natively (ListItem::isHeader: "never
// selected or focused"), so no extra bookkeeping is needed here to keep
// keyboard/button navigation moving between real rows only. A section with
// no jobs in it is omitted entirely rather than shown empty.
void buildInboxListItems(const store::JobIndex& jobs, freeink::ui::ListItem* items, size_t maxJobs, size_t& outCount,
                          char (*labelBuf)[80], char (*headerBuf)[24]) {
  outCount = 0;
  size_t headerSlot = 0;
  size_t jobsAdded = 0;
  for (size_t s = 0; s < kJobSectionCount; s++) {
    size_t sectionStart = outCount;
    for (size_t i = 0; i < jobs.count() && jobsAdded < maxJobs; i++) {
      const store::JobEntry& e = jobs.at(i);
      if (e.status != kJobSections[s].status) continue;
      std::snprintf(labelBuf[outCount], 80, "[%s] %s (%u pg)", statusGlyph(e.status), e.title,
                    static_cast<unsigned>(e.pageCount));
      items[outCount] = freeink::ui::ListItem{};
      items[outCount].label = labelBuf[outCount];
      items[outCount].actionValue = static_cast<int32_t>(i);
      outCount++;
      jobsAdded++;
    }
    if (outCount == sectionStart) continue;  // nothing in this section -- skip its header too

    std::snprintf(headerBuf[headerSlot], 24, "%s (%zu)", kJobSections[s].header, outCount - sectionStart);
    // Shift this section's rows down one slot and insert the header at
    // sectionStart -- simpler than a second pass since sections are small
    // (kMaxInboxJobs total across all of them) and this only runs once per
    // render, not per frame of an animation.
    for (size_t i = outCount; i > sectionStart; i--) items[i] = items[i - 1];
    items[sectionStart] = freeink::ui::ListItem{};
    items[sectionStart].label = headerBuf[headerSlot];
    items[sectionStart].actionValue = -1;  // sentinel: never a real job index
    items[sectionStart].isHeader = true;
    outCount++;
    headerSlot++;
  }
}

// Shown in place of the job list when the inbox is empty -- the wakeink-
// style idle screen: the cached next calendar event (config::CalendarCache,
// refreshed every sync pass -- see sync/SyncManager.cpp's calendar sync
// call and main.cpp's runSyncPass()) if one is configured and known,
// otherwise the plain "print something" message this screen always showed
// before calendar support existed.
//
// Known limitation: this firmware never configures a timezone (no NTP-
// synced offset, no TZ env var -- see sync/SyncManager.cpp's syncClock()),
// so times shown here are UTC, not the user's local time.
const char* idleScreenMessage(const InboxUiState& state) {
  static char body[192];
  if (state.nextEvent == nullptr || !state.nextEvent->hasEvent) {
    return "Inbox empty. Print something from any computer on your network, then wake this device.";
  }

  struct tm tmv;
  time_t start = state.nextEvent->start;
  gmtime_r(&start, &tmv);  // global, not std:: -- this file includes <time.h>, not <ctime>
  char timeStr[48];
  if (state.nextEvent->allDay) {
    strftime(timeStr, sizeof(timeStr), "%a %b %d (all day, UTC)", &tmv);
  } else {
    strftime(timeStr, sizeof(timeStr), "%a %b %d, %H:%M UTC", &tmv);
  }

  std::snprintf(body, sizeof(body), "Next: %s\n%s\n\nInbox empty. Print something to see it here.",
                state.nextEvent->title, timeStr);
  return body;
}

void homeScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);

  char subtitle[64];
  if (state.hasSyncedOnce) {
    std::snprintf(subtitle, sizeof(subtitle), "%d new * last sync +%d/-%d", state.lastSyncSummary.newJobsDownloaded,
                  state.lastSyncSummary.approvalsSynced, state.lastSyncSummary.approvalsFailedSync);
  } else {
    std::snprintf(subtitle, sizeof(subtitle), "%zu documents", state.jobs->count());
  }
  screen.header("Print Inbox", subtitle);

  static freeink::ui::ListItem items[store::kMaxInboxJobs + kJobSectionCount];
  static char labels[store::kMaxInboxJobs][80];
  static char headers[kJobSectionCount][24];
  size_t count = 0;
  buildInboxListItems(*state.jobs, items, store::kMaxInboxJobs, count, labels, headers);

  // Pick the row matching state.selectedJobIndex; if there isn't one (first
  // render, or the previously selected job is gone), fall back to the
  // first real row -- never a header (actionValue=-1 sentinel, and headers
  // aren't focusable anyway).
  int16_t selectedRow = 0;
  int16_t firstRealRow = -1;
  bool found = false;
  for (size_t i = 0; i < count && !found; i++) {
    if (items[i].isHeader) continue;
    if (firstRealRow < 0) firstRealRow = static_cast<int16_t>(i);
    if (items[i].actionValue == state.selectedJobIndex) {
      selectedRow = static_cast<int16_t>(i);
      found = true;
    }
  }
  if (!found && firstRealRow >= 0) {
    selectedRow = firstRealRow;
    state.selectedJobIndex = items[firstRealRow].actionValue;
  }

  const freeink::ui::FooterAction footer[] = {
      {.label = "Open", .action = ActionOpenJob},
      {.label = "Sync Now", .action = ActionSyncNow},
      {.label = "Web UI", .action = ActionOpenWebUiChoice},
      {.label = "Settings", .action = ActionOpenSettings},
  };
  screen.footer(footer, 4);

  if (count == 0) {
    screen.popup(idleScreenMessage(state));
  } else {
    screen.list(items, count, selectedRow, ActionOpenJob);
  }
}

void actionMenuScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);
  const store::JobEntry* job =
      (state.selectedJobIndex >= 0) ? &state.jobs->at(static_cast<size_t>(state.selectedJobIndex)) : nullptr;

  screen.header(job ? job->title : "Document", "Choose an action");

  // Row actionValue 0-3 maps to Print/Keep/Delete/Cancel, and 4 (shown only
  // when the job has a landscape variant) toggles the reader's view mode;
  // the single ActionMenuRowSelect handler below switches on event.value —
  // the same "list fires one action, the handler reads event.value" pattern
  // docs/freeink-ui.md's minimal example uses for its book list
  // (`state.selected = event.value` in handleOpen), rather than trying to
  // wire a distinct action id per row.
  bool hasLandscape = job != nullptr && job->landscapeXtcPath[0] != '\0';
  freeink::ui::ListItem items[5] = {
      {.label = "Print", .actionValue = 0},
      {.label = "Keep", .actionValue = 1},
      {.label = "Delete", .actionValue = 2},
      {.label = "Cancel", .actionValue = 3},
      {.label = state.landscapeView ? "View: Portrait" : "View: Landscape", .actionValue = 4},
  };
  static int16_t selected = 0;
  int16_t rowCount = hasLandscape ? 5 : 4;
  // `selected` persists across renders so the cursor survives re-renders
  // of the same open menu, but the row count now varies per job (5 with a
  // landscape variant, 4 without) -- clamp rather than let a stale index
  // from a previous job's 5-row menu be fed into this one's 4-row list.
  if (selected >= rowCount) selected = 0;
  screen.list(items, rowCount, selected, ActionMenuRowSelect);

  const freeink::ui::FooterAction footer[] = {
      {.label = "Cancel", .action = ActionCancelMenu},
  };
  screen.footer(footer, 1);
}

// Entry point for the on-device web UI (docs/architecture.md "On-device
// Web UI") — an explicit choice between joining the home Wi-Fi or
// broadcasting this device's own hotspot, not a silent fallback between
// them (see ui/WebUiServer.h's header comment for why). Same
// list-of-options idiom as actionMenuScreen above.
void webUiChoiceScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);

  screen.header("Web UI", state.webUiStartError ? state.webUiStartError : "View the queue from a phone");
  state.webUiStartError = nullptr;  // shown once

  freeink::ui::ListItem items[3] = {
      {.label = "Use Wi-Fi", .actionValue = 0},
      {.label = "Use Hotspot", .actionValue = 1},
      {.label = "Cancel", .actionValue = 2},
  };
  static int16_t selected = 0;
  screen.list(items, 3, selected, ActionWebUiChoiceRowSelect);

  const freeink::ui::FooterAction footer[] = {
      {.label = "Cancel", .action = ActionCancelWebUiChoice},
  };
  screen.footer(footer, 1);
}

// Shown while the web UI is running: connection info + the fresh PIN the
// phone must enter, and a Stop button. main.cpp's loop() keeps calling
// state.webUiServer.handleClient() while state.mode == ScreenMode::WebUi
// (see docs/architecture.md).
void webUiScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);

  char subtitle[96];
  if (state.webUiServer.mode() == WebUiMode::Hotspot) {
    std::snprintf(subtitle, sizeof(subtitle), "Hotspot: %s / %s", state.webUiServer.ssid(), state.webUiServer.password());
  } else {
    std::snprintf(subtitle, sizeof(subtitle), "Wi-Fi: %s", state.webUiServer.ssid());
  }
  screen.header("Web UI running", subtitle);

  char body[128];
  std::snprintf(body, sizeof(body), "On your phone, open http://%s/ and enter PIN %s",
                state.webUiServer.address().c_str(), state.webUiServer.pin());
  screen.popup(body);

  const freeink::ui::FooterAction footer[] = {
      {.label = "Stop", .action = ActionWebUiStop},
  };
  screen.footer(footer, 1);
}

void readerScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);
  if (state.selectedJobIndex < 0) {
    state.mode = ScreenMode::Inbox;
    return;
  }
  store::JobEntry& job = const_cast<store::JobEntry&>(state.jobs->at(static_cast<size_t>(state.selectedJobIndex)));

  if (!state.readerOpenForSelected) {
    state.reader.close();
    const char* path =
        (state.landscapeView && job.landscapeXtcPath[0] != '\0') ? job.landscapeXtcPath : job.xtcPath;
    state.readerOpenForSelected = state.reader.open(path);
    state.currentPage = 0;
  }

  if (state.readerOpenForSelected && state.framebuffer != nullptr) {
    xtc::RenderResult result = state.reader.renderPageToFramebuffer(
        state.currentPage, state.framebuffer, state.framebufferSize, state.panelWidth, state.panelHeight);
    if (result != xtc::RenderResult::Ok) {
      screen.popup("This page could not be rendered (unsupported or corrupt data).");
    }
  } else {
    screen.popup("Could not open this document from SD.");
  }

  // Drawn AFTER the raw page write, on top of it — see InboxUI.h header
  // comment for why this ordering is deliberate.
  char footerLabel[32];
  uint16_t total = state.readerOpenForSelected ? state.reader.pageCount() : 0;
  std::snprintf(footerLabel, sizeof(footerLabel), "%u / %u%s", state.currentPage + 1, total,
                state.landscapeView ? " L" : "");
  const freeink::ui::FooterAction footer[] = {
      {.label = "Prev", .action = ActionPrevPage},
      {.label = footerLabel, .action = ActionShowActionMenu},
      {.label = "Next", .action = ActionNextPage},
      {.label = "Back", .action = ActionBack},
  };
  screen.footer(footer, 4);
}

const char* settingsTabName(SettingsTab tab) {
  switch (tab) {
    case SettingsTab::Wifi:
      return "Wi-Fi";
    case SettingsTab::SyncRelay:
      return "Sync & Relay";
    case SettingsTab::Display:
      return "Display";
    case SettingsTab::Calendar:
      return "Calendar";
    case SettingsTab::DeviceInfo:
      return "Device Info";
    default:
      return "";
  }
}

// Saved networks: view + remove only. Adding one needs typed SSID/password
// entry this device has no keyboard for -- see WebUiServer.h's on-device
// web UI, which is where that belongs instead (a phone's own keyboard).
void settingsWifiTab(App::ScreenType& screen, InboxUiState& state) {
  config::WifiStore& store = config::WifiStore::instance();

  if (store.count() == 0) {
    screen.popup("No saved Wi-Fi networks. Add one from the on-device Web UI (Inbox screen).");
    return;
  }

  static freeink::ui::ListItem items[config::kMaxWifiNetworks];
  static char labels[config::kMaxWifiNetworks][config::kMaxSsidLen + 1];
  size_t count = store.count();
  for (size_t i = 0; i < count; i++) {
    const config::WifiCredential& cred = store.at(i);
    std::snprintf(labels[i], sizeof(labels[i]), "%s", cred.ssid);
    items[i] = freeink::ui::ListItem{};
    items[i].label = labels[i];
    items[i].subtitle = std::strcmp(store.lastConnectedSsid(), cred.ssid) == 0 ? "Last connected" : nullptr;
    items[i].actionValue = static_cast<int32_t>(i);
  }

  static int16_t selected = 0;
  if (selected >= static_cast<int16_t>(count)) selected = 0;
  screen.list(items, count, selected, ActionSettingsWifiRowSelect);

  if (state.settingsWifiRemoveIndex >= 0 && state.settingsWifiRemoveIndex < static_cast<int16_t>(count)) {
    const config::WifiCredential& target = store.at(static_cast<size_t>(state.settingsWifiRemoveIndex));
    freeink::ui::OptionDialogProps dialog;
    dialog.title = "Remove network?";
    dialog.headline = target.ssid;
    static const freeink::ui::DialogOption options[] = {
        {.label = "Remove", .action = ActionSettingsWifiConfirmRemove},
        {.label = "Cancel", .action = ActionSettingsWifiCancelRemove},
    };
    dialog.options = options;
    dialog.optionCount = 2;
    screen.dialog(dialog);
  }
}

// Read-only: everything here is either provisioned by
// pi-server/tools/pair_device.py (device id/name, Pi/relay URLs) or a
// result of the last sync pass -- nothing on this tab is editable
// on-device, matching how config.py's own RUNTIME_OVERRIDABLE_FIELDS
// draws the same "what's live-editable" line on the Pi side.
void settingsSyncRelayTab(App::ScreenType& screen, InboxUiState& state) {
  char body[320];
  const config::DeviceConfigData* cfg = state.deviceConfig;
  if (cfg == nullptr || !cfg->loaded) {
    screen.popup("Not paired yet. See docs/setup-x4.md.");
    return;
  }

  int written = std::snprintf(body, sizeof(body), "Device: %s\nPaired to: %s\nRelay: %s",
                               cfg->deviceName[0] ? cfg->deviceName : cfg->deviceId, cfg->piBaseUrl,
                               cfg->hasRelay ? cfg->relayBaseUrl : "not configured");
  if (state.hasSyncedOnce && written > 0 && static_cast<size_t>(written) < sizeof(body)) {
    std::snprintf(body + written, sizeof(body) - static_cast<size_t>(written),
                  "\nLast sync: %d new, %d approval(s) synced%s", state.lastSyncSummary.newJobsDownloaded,
                  state.lastSyncSummary.approvalsSynced, state.lastSyncSummary.usedRelay ? " (via relay)" : "");
  }
  screen.popup(body);
}

void settingsDisplayTab(App::ScreenType& screen, InboxUiState& state) {
  freeink::ui::ListItem item{};
  item.label = "Default view";
  item.toggle = true;
  item.toggleChecked = state.appSettings != nullptr && state.appSettings->defaultLandscapeView;
  item.actionValue = 0;
  screen.list(&item, 1, 0, ActionSettingsDisplayRowSelect);
}

// Wake-before-event / wake-at-event-end reminders (calendar/WakeSchedule.h)
// -- each row toggles/cycles a config::AppSettingsData field and saves
// immediately, same pattern as settingsDisplayTab's toggle. Meaningless
// (and harmless to leave configured) until /system/calendars.json is set
// up -- see docs/setup-x4.md "Calendar idle screen".
void settingsCalendarTab(App::ScreenType& screen, InboxUiState& state) {
  if (state.appSettings == nullptr) return;
  const config::AppSettingsData& settings = *state.appSettings;

  char leadValue[16];
  std::snprintf(leadValue, sizeof(leadValue), "%u min", settings.calendarWakeLeadMinutes);

  freeink::ui::ListItem items[3] = {
      {.label = "Wake before start",
       .actionValue = 0,
       .toggle = true,
       .toggleChecked = settings.calendarWakeBeforeStart},
      {.label = "Lead time", .value = leadValue, .actionValue = 1},
      {.label = "Wake at event end", .actionValue = 2, .toggle = true, .toggleChecked = settings.calendarWakeAtEnd},
  };
  static int16_t selected = 0;
  screen.list(items, 3, selected, ActionSettingsCalendarRowSelect);
}

void settingsDeviceInfoTab(App::ScreenType& screen) {
  BatteryMonitor::Status battery = BatteryMonitor().readStatus();
  uint64_t sdTotal = SdMan.sdTotalBytes();
  uint64_t sdUsed = SdMan.sdUsedBytes();

  char batteryStr[16];
  if (battery.percentageKnown) {
    std::snprintf(batteryStr, sizeof(batteryStr), "%u%%", static_cast<unsigned>(battery.percentage));
  } else {
    std::snprintf(batteryStr, sizeof(batteryStr), "unknown");
  }

  char body[256];
  std::snprintf(body, sizeof(body), "Firmware: %s\nBattery: %s\nStorage: %llu / %llu MB used",
                config::kFirmwareVersion, batteryStr, static_cast<unsigned long long>(sdUsed / (1024 * 1024)),
                static_cast<unsigned long long>(sdTotal / (1024 * 1024)));
  screen.popup(body);
}

void settingsScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);

  screen.header("Settings", settingsTabName(state.settingsTab));

  switch (state.settingsTab) {
    case SettingsTab::Wifi:
      settingsWifiTab(screen, state);
      break;
    case SettingsTab::SyncRelay:
      settingsSyncRelayTab(screen, state);
      break;
    case SettingsTab::Display:
      settingsDisplayTab(screen, state);
      break;
    case SettingsTab::Calendar:
      settingsCalendarTab(screen, state);
      break;
    case SettingsTab::DeviceInfo:
    default:
      settingsDeviceInfoTab(screen);
      break;
  }

  const freeink::ui::FooterAction footer[] = {
      {.label = "< Tab", .action = ActionSettingsPrevTab},
      {.label = "Tab >", .action = ActionSettingsNextTab},
      {.label = "Back", .action = ActionSettingsBack},
  };
  screen.footer(footer, 3);
}

// Rendered exactly once on a Timer wake that lands within a configured
// calendar-reminder window (Settings > Calendar tab), then main.cpp goes
// straight back to sleep -- see ScreenMode::CalendarReminder's comment in
// InboxUI.h. No footer: nobody is expected to be holding the device for a
// background timer wake, and the e-paper panel holds this frame with no
// power until the next wake redraws it.
void calendarReminderScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);
  bool atEnd = state.calendarReminderKind == CalendarReminderKind::AtEnd;
  screen.header("Reminder", atEnd ? "Event ended" : "Starting soon");

  if (state.nextEvent == nullptr || !state.nextEvent->hasEvent) {
    screen.popup("");
    return;
  }

  struct tm tmv;
  time_t when = atEnd ? state.nextEvent->end : state.nextEvent->start;
  gmtime_r(&when, &tmv);  // global, not std:: -- this file includes <time.h>, not <ctime>
  char timeStr[48];
  strftime(timeStr, sizeof(timeStr), "%a %b %d, %H:%M UTC", &tmv);

  static char body[160];
  std::snprintf(body, sizeof(body), "%s\n%s", state.nextEvent->title, timeStr);
  screen.popup(body);
}

void screenRouter(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);
  switch (state.mode) {
    case ScreenMode::CalendarReminder:
      calendarReminderScreen(screen, userPtr);
      return;
    case ScreenMode::Reader:
      readerScreen(screen, userPtr);
      return;
    case ScreenMode::ActionMenu:
      actionMenuScreen(screen, userPtr);
      return;
    case ScreenMode::WebUiChoice:
      webUiChoiceScreen(screen, userPtr);
      return;
    case ScreenMode::WebUi:
      webUiScreen(screen, userPtr);
      return;
    case ScreenMode::Settings:
      settingsScreen(screen, userPtr);
      return;
    case ScreenMode::Inbox:
    case ScreenMode::Status:
    default:
      homeScreen(screen, userPtr);
      return;
  }
}

uint32_t nowEpoch() { return static_cast<uint32_t>(time(nullptr)); }

// Thin wrapper: supplies the SoC-specific id/timestamp, then defers to
// store::enqueueApproval() (ApprovalOutbox.h) for the actual durable-
// before-network logic — the on-device web UI (ui/WebUiServer.cpp) calls
// the exact same shared function, not a duplicate of this. Behavior is
// unchanged from before this was split out: AlreadyPending/OutboxFull
// still silently no-op and leave the screen where it was (the UI already
// prevents queuing a 2nd action on a job with one pending, and a full
// outbox is surfaced elsewhere) — only a real Ok persists to SD and
// returns to the Inbox.
void enqueueApproval(InboxUiState& state, store::ApprovalAction action) {
  if (state.selectedJobIndex < 0) return;
  store::JobEntry& job = const_cast<store::JobEntry&>(state.jobs->at(static_cast<size_t>(state.selectedJobIndex)));

  char approvalId[store::kApprovalIdLen + 1];
  fwrand::randomHex(approvalId, store::kApprovalIdLen);

  store::EnqueueResult result =
      store::enqueueApproval(*state.jobs, *state.outbox, job.jobId, action, approvalId, nowEpoch());
  if (result != store::EnqueueResult::Ok) return;

  store::saveApprovalOutbox(*state.outbox);  // durable BEFORE any network attempt — see ApprovalOutbox.h
  store::saveJobIndex(*state.jobs);
  state.mode = ScreenMode::Inbox;
}

}  // namespace

void initApp(App& app, InboxUiState& state) {
  app.setScreen(screenRouter, &state);

  // state.jobs/outbox/deviceConfig are already wired by main.cpp's
  // setup() before this call — see InboxUI.h's InboxUiState comment for
  // why webUiServer takes these post-construction rather than as
  // reference members.
  state.webUiServer.attach(state.jobs, state.outbox, state.deviceConfig, state.panelWidth, state.panelHeight,
                            state.wakeMillis);

  app.on(
      ActionOpenJob,
      [](const freeink::ui::ActionEvent& event, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.selectedJobIndex = event.value;
        s.readerOpenForSelected = false;
        // Falls back to portrait regardless of this default when the job has
        // no landscape variant -- readerScreen() only honors landscapeView
        // once it's confirmed job.landscapeXtcPath is non-empty.
        s.landscapeView = s.appSettings != nullptr && s.appSettings->defaultLandscapeView;
        s.mode = ScreenMode::Reader;
      },
      &state);

  app.on(
      ActionBack,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.reader.close();
        s.readerOpenForSelected = false;
        s.mode = ScreenMode::Inbox;
      },
      &state);

  app.on(
      ActionNextPage,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.readerOpenForSelected && s.currentPage + 1 < s.reader.pageCount()) s.currentPage++;
      },
      &state);

  app.on(
      ActionPrevPage,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.currentPage > 0) s.currentPage--;
      },
      &state);

  app.on(
      ActionShowActionMenu,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::ActionMenu;
      },
      &state);

  app.on(
      ActionCancelMenu,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::Reader;
      },
      &state);

  app.on(
      ActionMenuRowSelect,
      [](const freeink::ui::ActionEvent& event, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        switch (event.value) {
          case 0:
            enqueueApproval(s, store::ApprovalAction::Print);
            break;
          case 1:
            enqueueApproval(s, store::ApprovalAction::Keep);
            break;
          case 2:
            enqueueApproval(s, store::ApprovalAction::Delete);
            break;
          case 4:
            // Toggle landscape/portrait view -- only reachable when
            // actionMenuScreen() offered this row (job has a landscape
            // variant). Force a reopen so readerScreen() picks up the
            // other file.
            s.landscapeView = !s.landscapeView;
            s.readerOpenForSelected = false;
            s.currentPage = 0;
            s.mode = ScreenMode::Reader;
            break;
          default:
            s.mode = ScreenMode::Reader;  // Cancel (3), or anything else
            break;
        }
      },
      &state);

  app.on(
      ActionSyncNow,
      [](const freeink::ui::ActionEvent&, void* userPtr) { static_cast<InboxUiState*>(userPtr)->requestSyncNow = true; },
      &state);

  app.on(
      ActionOpenWebUiChoice,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::WebUiChoice;
      },
      &state);

  app.on(
      ActionWebUiChoiceRowSelect,
      [](const freeink::ui::ActionEvent& event, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        switch (event.value) {
          case 0:
            if (s.webUiServer.startStation()) {
              s.mode = ScreenMode::WebUi;
            } else {
              s.webUiStartError = "No known Wi-Fi network in range";
              // stays on WebUiChoice — webUiChoiceScreen shows the error once
            }
            break;
          case 1:
            if (s.webUiServer.startHotspot()) {
              s.mode = ScreenMode::WebUi;
            } else {
              s.webUiStartError = "Could not start hotspot";
            }
            break;
          default:
            s.mode = ScreenMode::Inbox;  // Cancel
            break;
        }
      },
      &state);

  app.on(
      ActionWebUiStop,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.webUiServer.stop();
        s.mode = ScreenMode::Inbox;
      },
      &state);

  app.on(
      ActionCancelWebUiChoice,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::Inbox;
      },
      &state);

  app.on(
      ActionOpenSettings,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.settingsWifiRemoveIndex = -1;  // dismiss any stale confirm from a previous visit
        s.mode = ScreenMode::Settings;
      },
      &state);

  app.on(
      ActionSettingsBack,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.settingsWifiRemoveIndex = -1;
        s.mode = ScreenMode::Inbox;
      },
      &state);

  app.on(
      ActionSettingsPrevTab,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.settingsWifiRemoveIndex = -1;
        uint8_t count = static_cast<uint8_t>(SettingsTab::kCount);
        uint8_t current = static_cast<uint8_t>(s.settingsTab);
        s.settingsTab = static_cast<SettingsTab>((current + count - 1) % count);
      },
      &state);

  app.on(
      ActionSettingsNextTab,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        s.settingsWifiRemoveIndex = -1;
        uint8_t count = static_cast<uint8_t>(SettingsTab::kCount);
        uint8_t current = static_cast<uint8_t>(s.settingsTab);
        s.settingsTab = static_cast<SettingsTab>((current + 1) % count);
      },
      &state);

  app.on(
      ActionSettingsWifiRowSelect,
      [](const freeink::ui::ActionEvent& event, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->settingsWifiRemoveIndex = static_cast<int16_t>(event.value);
      },
      &state);

  app.on(
      ActionSettingsWifiConfirmRemove,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        config::WifiStore& store = config::WifiStore::instance();
        if (s.settingsWifiRemoveIndex >= 0 && s.settingsWifiRemoveIndex < static_cast<int16_t>(store.count())) {
          // Copy the ssid first: remove() swaps the last entry into this
          // slot (see WifiStore::remove), so reading store.at(index) after
          // calling it would read the wrong network.
          char ssid[config::kMaxSsidLen + 1];
          std::snprintf(ssid, sizeof(ssid), "%s", store.at(static_cast<size_t>(s.settingsWifiRemoveIndex)).ssid);
          store.remove(ssid);
          store.save();
        }
        s.settingsWifiRemoveIndex = -1;
      },
      &state);

  app.on(
      ActionSettingsWifiCancelRemove,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->settingsWifiRemoveIndex = -1;
      },
      &state);

  app.on(
      ActionSettingsDisplayRowSelect,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.appSettings == nullptr) return;
        s.appSettings->defaultLandscapeView = !s.appSettings->defaultLandscapeView;
        config::AppSettings::instance().setDefaultLandscapeView(s.appSettings->defaultLandscapeView);
        config::AppSettings::instance().save();
      },
      &state);

  app.on(
      ActionSettingsCalendarRowSelect,
      [](const freeink::ui::ActionEvent& event, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.appSettings == nullptr) return;
        config::AppSettings& settings = config::AppSettings::instance();
        switch (event.value) {
          case 0:
            s.appSettings->calendarWakeBeforeStart = !s.appSettings->calendarWakeBeforeStart;
            settings.setCalendarWakeBeforeStart(s.appSettings->calendarWakeBeforeStart);
            break;
          case 1: {
            uint16_t current = s.appSettings->calendarWakeLeadMinutes;
            size_t next = 0;
            for (size_t i = 0; i < kCalendarLeadMinutePresetCount; i++) {
              if (kCalendarLeadMinutePresets[i] == current) {
                next = (i + 1) % kCalendarLeadMinutePresetCount;
                break;
              }
            }
            s.appSettings->calendarWakeLeadMinutes = kCalendarLeadMinutePresets[next];
            settings.setCalendarWakeLeadMinutes(s.appSettings->calendarWakeLeadMinutes);
            break;
          }
          case 2:
            s.appSettings->calendarWakeAtEnd = !s.appSettings->calendarWakeAtEnd;
            settings.setCalendarWakeAtEnd(s.appSettings->calendarWakeAtEnd);
            break;
          default:
            return;
        }
        settings.save();
      },
      &state);
}

}  // namespace ui
