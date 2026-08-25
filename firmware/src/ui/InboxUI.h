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

#include "config/DeviceConfig.h"
#include "store/ApprovalOutbox.h"
#include "store/JobStore.h"
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
};

struct InboxUiState {
  store::JobIndex* jobs = nullptr;
  store::ApprovalOutboxIndex* outbox = nullptr;
  const config::DeviceConfigData* deviceConfig = nullptr;

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

  sync::SyncSummary lastSyncSummary;
  bool hasSyncedOnce = false;

  // Set by main.cpp before each app.render() call so screen functions can
  // reach the raw framebuffer for the reader's direct page write.
  uint8_t* framebuffer = nullptr;
  size_t framebufferSize = 0;
  uint16_t panelWidth = 0;
  uint16_t panelHeight = 0;

  // Signals to main.cpp's loop, set by action handlers, cleared after
  // being acted on. Keeps SyncManager/SleepManager out of the UI layer.
  bool requestSyncNow = false;
  bool requestSleep = false;
};

// Builds the App instance and registers every action handler. Call once in
// setup() after the display/UI DisplayTarget are constructed.
void initApp(App& app, InboxUiState& state);

}  // namespace ui
