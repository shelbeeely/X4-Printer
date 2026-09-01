#pragma once
// On-device web UI: a manually-toggled, ephemeral HTTP server (plain
// WebServer, no TLS — see docs/security.md "On-device Web UI") that lets
// a phone browser view/manage the print queue, either by joining the
// home Wi-Fi (startStation) or by broadcasting this device's own hotspot
// (startHotspot) when away from any known network. This is a deliberate,
// scoped exception to "never accepts inbound connections" (see
// docs/architecture.md's "On-device Web UI" section) — off unless
// explicitly toggled on from the Inbox screen (ui/InboxUI.cpp), gated by
// a fresh PIN shown on the e-ink screen every time it's turned on, and
// torn down by the same idle timer that governs the rest of the UI (see
// main.cpp's loop()/goToSleep()).

#include <cstdint>

#include <SdFat.h>
#include <WString.h>
#include <WebServer.h>

#include "config/DeviceConfig.h"
#include "store/ApprovalOutbox.h"
#include "store/JobStore.h"

// Also serves tools/xtc-wasm/'s compiled WASM decoder (embedded via
// ui/XtcDecoderWasmData.h) and encoder (ui/XtcEncoderWasmData.h) and a
// raw-bytes route for a job's XTC file, so the job list page can
// decode/preview a page and encode a phone-picked photo into a new job
// entirely client-side — see ui/pages/joblist.html's script for the
// fetch/decode/encode/upload glue. Both on-device pages
// (ui/pages/login.html, ui/pages/joblist.html) and the WASM
// decoder/encoder .wasm/.js pairs are embedded gzip-compressed
// (ui/PagesData.h, ui/XtcDecoderWasmData.h, ui/XtcEncoderWasmData.h) and
// served via sendGzip() below — see ui/pages/generate_pages_header.py for
// why.
//
// POST /api/upload/xtc and POST /api/upload/original let that same page
// create a job directly on this device (no Pi involved) and later hand
// its original image bytes to the Pi on the next real sync — see
// docs/architecture.md's direct-upload section and
// sync/SyncManager.cpp's uploadPendingOriginals().

namespace ui {

enum class WebUiMode { Off, Station, Hotspot };

class WebUiServer {
 public:
  WebUiServer() = default;

  // Wires the job/outbox/device-config state this server reads and
  // mutates. Split from the constructor because a WebUiServer lives as a
  // plain member of InboxUiState (default-constructed as part of a
  // global, the same reason InboxUiState itself uses pointers wired
  // after construction rather than reference members) — call once from
  // ui::initApp(), after InboxUiState's own pointers are set in
  // main.cpp's setup().
  // panelWidth/panelHeight/wakeMillis feed the diagnostics route
  // (handleApiDiag()) only — the device already computes these for its own
  // framebuffer sizing and idle timer (main.cpp's setup()), so this is
  // exposing existing values, not tracking new state on this class's
  // behalf.
  void attach(store::JobIndex* jobs, store::ApprovalOutboxIndex* outbox, const config::DeviceConfigData* deviceConfig,
              uint16_t panelWidth, uint16_t panelHeight, uint32_t wakeMillis);

  // Joins a known Wi-Fi network (net::WifiManager::connect(), same saved
  // credentials the normal sync pass uses) and starts the server on the
  // resulting station IP. Returns false (server not started, mode stays
  // Off) if no known network is in range within timeoutMs.
  bool startStation(uint32_t timeoutMs = 15000);

  // Broadcasts this device's own hotspot (a fresh random SSID/password
  // each time) and starts the server on the AP gateway IP. Returns false
  // only if the radio itself fails to start.
  bool startHotspot();

  // Stops the HTTP server and tears down whichever radio mode was
  // active. Safe to call when not active (isActive() == false).
  void stop();

  bool isActive() const { return mode_ != WebUiMode::Off; }
  WebUiMode mode() const { return mode_; }

  // Services at most the currently pending client request. Cheap/non-
  // blocking when idle (WebServer's own behavior) — call every
  // main.cpp loop() pass while isActive().
  void handleClient();

  // True (and clears the flag) if handleClient() has serviced a request
  // since the last call — main.cpp feeds this into its idle timer so an
  // actively-browsing phone (the page's periodic /api/status poll keeps
  // this ticking) keeps the device from sleeping mid-session, with no
  // separate timeout constant for this mode.
  bool consumeActivityFlag();

  // Status accessors for ui/InboxUI.cpp's webUiScreen().
  const char* ssid() const { return ssid_; }
  const char* password() const { return password_; }  // hotspot mode only; empty in station mode
  String address() const;
  const char* pin() const { return pin_; }

 private:
  store::JobIndex* jobs_ = nullptr;
  store::ApprovalOutboxIndex* outbox_ = nullptr;
  const config::DeviceConfigData* deviceConfig_ = nullptr;
  uint16_t panelWidth_ = 0;
  uint16_t panelHeight_ = 0;
  uint32_t wakeMillis_ = 0;

  WebServer server_{80};
  WebUiMode mode_ = WebUiMode::Off;
  bool activityFlag_ = false;
  // WebServer::on() has no matching "remove handler" call, so routes are
  // registered exactly once (guarded by this flag) rather than on every
  // start*() — otherwise repeated toggle-on/off cycles (an explicitly
  // documented normal usage pattern, see docs/setup-x4.md) would leak a
  // new handler registration into the server's internal list each time.
  bool routesRegistered_ = false;

  char ssid_[40] = {0};
  char password_[16] = {0};
  char pin_[7] = {0};        // 6 digits + NUL
  char sessionToken_[33] = {0};  // 32 hex chars + NUL, set on successful /login

  // Streaming-upload state for POST /api/upload/xtc, carried from the
  // per-chunk upload handler (invoked repeatedly while the multipart body
  // streams in) to the completion handler that runs once the whole
  // request has been read — see WebUiServer.cpp's handleUploadXtcData()/
  // handleUploadXtcComplete() for why this can't just be local state.
  bool uploadXtcAuthorized_ = false;
  bool uploadXtcTooLarge_ = false;
  const char* uploadXtcError_ = nullptr;
  FsFile uploadXtcFile_;
  char uploadXtcJobId_[store::kJobIdLen + 1] = {0};
  char uploadXtcPath_[store::kPathLen + 1] = {0};
  uint32_t uploadXtcBytes_ = 0;

  // Same shape, for POST /api/upload/original (see handleUploadOriginalData()/
  // handleUploadOriginalComplete()).
  bool uploadOriginalAuthorized_ = false;
  bool uploadOriginalTooLarge_ = false;
  const char* uploadOriginalError_ = nullptr;
  FsFile uploadOriginalFile_;
  char uploadOriginalJobId_[store::kJobIdLen + 1] = {0};
  char uploadOriginalPath_[store::kPathLen + 1] = {0};
  char uploadOriginalMime_[24] = {0};
  uint32_t uploadOriginalBytes_ = 0;

  void beginCommon();
  void generateSessionSecrets();
  // Not const: WebServer::hasHeader()/header() aren't guaranteed const in
  // the vendored library (unverifiable in this environment — freeink-sdk
  // isn't checked out here), so this is deliberately non-const rather
  // than assuming.
  bool requestHasValidSession();
  void markActivity();

  void handleRoot();
  void handleLogin();
  void handleApiStatus();
  void handleApiDiag();
  void handleApiJobsGet();
  void handleApiJobsPost();
  void handleApiJobXtc();
  void handleXtcDecoderWasm();
  void handleXtcDecoderJs();
  void handleXtcEncoderWasm();
  void handleXtcEncoderJs();
  void handleUploadXtcData();
  void handleUploadXtcComplete();
  void handleUploadOriginalData();
  void handleUploadOriginalComplete();
  void handleNotFound();

  // Shared by every route serving one of the gzip-embedded static assets
  // (firmware/src/ui/PagesData.h, XtcDecoderWasmData.h) — every browser
  // capable of running this UI's client-side WASM decode also
  // unconditionally supports gzip response decoding, so this is used
  // without checking the request's Accept-Encoding header.
  void sendGzip(int code, const char* contentType, const unsigned char* data, size_t len);
};

}  // namespace ui
