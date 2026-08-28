#include "ui/InboxUI.h"

#include <ArduinoJson.h>  // pulled transitively by other headers; kept explicit for clarity
#include <time.h>

#include <cstdio>
#include <cstring>

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
};

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

// Builds the list rows shown on the Inbox screen. Deleted jobs are hidden
// (they still exist on disk pending sync/retention, see
// docs/architecture.md's data model) but everything else is visible so the
// user can see what's already been actioned, not just what's new.
void buildInboxListItems(const store::JobIndex& jobs, freeink::ui::ListItem* items, size_t maxItems, size_t& outCount,
                          char (*labelBuf)[80]) {
  outCount = 0;
  for (size_t i = 0; i < jobs.count() && outCount < maxItems; i++) {
    const store::JobEntry& e = jobs.at(i);
    if (e.status == store::JobStatus::ApprovedDelete) continue;
    std::snprintf(labelBuf[outCount], 80, "[%s] %s (%u pg)", statusGlyph(e.status), e.title,
                  static_cast<unsigned>(e.pageCount));
    items[outCount] = freeink::ui::ListItem{};
    items[outCount].label = labelBuf[outCount];
    items[outCount].actionValue = static_cast<int32_t>(i);
    outCount++;
  }
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

  static freeink::ui::ListItem items[store::kMaxInboxJobs];
  static char labels[store::kMaxInboxJobs][80];
  size_t count = 0;
  buildInboxListItems(*state.jobs, items, store::kMaxInboxJobs, count, labels);

  if (state.selectedJobIndex < 0 && count > 0) state.selectedJobIndex = items[0].actionValue;
  int16_t selectedRow = 0;
  for (size_t i = 0; i < count; i++) {
    if (items[i].actionValue == state.selectedJobIndex) {
      selectedRow = static_cast<int16_t>(i);
      break;
    }
  }

  const freeink::ui::FooterAction footer[] = {
      {.label = "Open", .action = ActionOpenJob},
      {.label = "Sync Now", .action = ActionSyncNow},
      {.label = "Web UI", .action = ActionOpenWebUiChoice},
  };
  screen.footer(footer, 3);

  if (count == 0) {
    screen.popup("Inbox empty. Print something from any computer on your network, then wake this device.");
  } else {
    screen.list(items, count, selectedRow, ActionOpenJob);
  }
}

void actionMenuScreen(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);
  const store::JobEntry* job =
      (state.selectedJobIndex >= 0) ? &state.jobs->at(static_cast<size_t>(state.selectedJobIndex)) : nullptr;

  screen.header(job ? job->title : "Document", "Choose an action");

  // Row actionValue 0-3 maps to Print/Keep/Delete/Cancel; the single
  // ActionMenuRowSelect handler below switches on event.value — the same
  // "list fires one action, the handler reads event.value" pattern
  // docs/freeink-ui.md's minimal example uses for its book list
  // (`state.selected = event.value` in handleOpen), rather than trying to
  // wire a distinct action id per row.
  freeink::ui::ListItem items[4] = {
      {.label = "Print", .actionValue = 0},
      {.label = "Keep", .actionValue = 1},
      {.label = "Delete", .actionValue = 2},
      {.label = "Cancel", .actionValue = 3},
  };
  static int16_t selected = 0;
  screen.list(items, 4, selected, ActionMenuRowSelect);

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
    state.readerOpenForSelected = state.reader.open(job.xtcPath);
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
  std::snprintf(footerLabel, sizeof(footerLabel), "%u / %u", state.currentPage + 1, total);
  const freeink::ui::FooterAction footer[] = {
      {.label = "Prev", .action = ActionPrevPage},
      {.label = footerLabel, .action = ActionShowActionMenu},
      {.label = "Next", .action = ActionNextPage},
      {.label = "Back", .action = ActionBack},
  };
  screen.footer(footer, 4);
}

void screenRouter(App::ScreenType& screen, void* userPtr) {
  auto& state = *static_cast<InboxUiState*>(userPtr);
  switch (state.mode) {
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
          default:
            s.mode = ScreenMode::Reader;  // Cancel
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
}

}  // namespace ui
