#include "ui/InboxUI.h"

#include <ArduinoJson.h>  // pulled transitively by other headers; kept explicit for clarity
#include <time.h>

#include <cstdio>
#include <cstring>

#include "store/AtomicJsonFile.h"

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
  };
  screen.footer(footer, 2);

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
    case ScreenMode::Inbox:
    case ScreenMode::Status:
    default:
      homeScreen(screen, userPtr);
      return;
  }
}

uint32_t nowEpoch() { return static_cast<uint32_t>(time(nullptr)); }

// Generates a uuid4-hex-shaped id from the ESP32 hardware RNG
// (esp_random(), true entropy — not a PRNG seeded from millis()). Good
// enough uniqueness for an approval_id: it only needs to never collide
// with another id THIS device generates, since the server dedups per
// approval_id globally and a collision would just look like a (harmless)
// retried duplicate, not a wrong action.
void generateId(char* out, size_t outSize) {
  static const char* hex = "0123456789abcdef";
  for (size_t i = 0; i + 1 < outSize && i < 32; i++) {
    out[i] = hex[esp_random() & 0xF];
  }
  out[outSize > 32 ? 32 : outSize - 1] = '\0';
}

void enqueueApproval(InboxUiState& state, store::ApprovalAction action) {
  if (state.selectedJobIndex < 0) return;
  store::JobEntry& job = const_cast<store::JobEntry&>(state.jobs->at(static_cast<size_t>(state.selectedJobIndex)));

  if (state.outbox->hasPendingForJob(job.jobId)) {
    return;  // already has an unsynced approval queued — see ApprovalOutbox.h
  }
  if (state.outbox->full()) {
    return;  // UI should have surfaced "sync before approving more" already
  }

  store::ApprovalEntry entry;
  generateId(entry.approvalId, sizeof(entry.approvalId));
  std::strncpy(entry.jobId, job.jobId, sizeof(entry.jobId) - 1);
  entry.action = action;
  entry.createdAt = nowEpoch();
  entry.synced = false;

  if (state.outbox->append(entry)) {
    store::saveApprovalOutbox(*state.outbox);  // durable BEFORE any network attempt — see ApprovalOutbox.h

    store::JobStatus newStatus = store::JobStatus::Downloaded;
    switch (action) {
      case store::ApprovalAction::Print:
        newStatus = store::JobStatus::ApprovedPrint;
        break;
      case store::ApprovalAction::Keep:
        newStatus = store::JobStatus::ApprovedKeep;
        break;
      case store::ApprovalAction::Delete:
        newStatus = store::JobStatus::ApprovedDelete;
        break;
    }
    state.jobs->setStatus(job.jobId, newStatus);
    store::saveJobIndex(*state.jobs);
  }

  state.mode = ScreenMode::Inbox;
}

}  // namespace

void initApp(App& app, InboxUiState& state) {
  app.setScreen(screenRouter, &state);

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
}

}  // namespace ui
