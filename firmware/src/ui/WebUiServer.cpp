#include "ui/WebUiServer.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <BatteryMonitor.h>
#include <MemoryManager.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "config/CalendarCache.h"
#include "net/WifiManager.h"
#include "ui/PagesData.h"
#include "ui/TimelineMerge.h"
#include "ui/XtcDecoderWasmData.h"
#include "ui/XtcEncoderWasmData.h"
#include "util/Random.h"
#include "xtc/XtcReader.h"

namespace ui {

namespace {

// PIN/password generators are distinct formats (decimal digits;
// screen-readable charset) from fwrand::randomHex() (util/Random.h,
// shared with ui/InboxUI.cpp's approval-id generation), so they stay
// local here rather than being folded into that shared helper.

// Matches xtc::XtcReader.cpp's own kBulkChunkBytes — bounded RAM regardless
// of job size, no full-file buffer.
constexpr size_t kXtcStreamChunkBytes = 2048;

// Generous upper bounds for the two direct-upload routes below, just to
// stop a buggy/malicious client from filling the SD card — not tuned to
// any real expected size. A single 800x480 1bpp XTC page tops out well
// under 64KB; a phone camera photo can legitimately run into the low tens
// of megabytes, so that cap is far larger (and matches the sync-side cap
// pi-server's new upload endpoint enforces — see docs/protocol.md §1.7).
constexpr uint32_t kMaxXtcUploadBytes = 512u * 1024u;
constexpr uint32_t kMaxOriginalUploadBytes = 20u * 1024u * 1024u;

void randomPin(char* out) {  // out must be 7 bytes
  uint32_t v = esp_random() % 1000000u;
  std::snprintf(out, 7, "%06u", static_cast<unsigned>(v));
}

void randomPassword(char* out, size_t n) {  // out must be n+1 bytes; WPA2 needs 8+ chars
  static const char* kCharset = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";  // no 0/O/1/I/L — easier to read off-screen
  size_t len = std::strlen(kCharset);
  for (size_t i = 0; i < n; i++) out[i] = kCharset[esp_random() % len];
  out[n] = '\0';
}

// Human-readable job status for the web UI's job cards. Safe to change from
// the raw enum ordinal the API used to send: the server and its only
// consumer (ui/pages/joblist.html) always ship from the same firmware
// image, so there's no external client depending on the old int shape.
const char* jobStatusLabel(store::JobStatus s) {
  switch (s) {
    case store::JobStatus::Downloaded:
      return "New";
    case store::JobStatus::ApprovedPrint:
      return "Printing";
    case store::JobStatus::ApprovedKeep:
      return "Kept";
    case store::JobStatus::ApprovedDelete:
      return "Deleted";
  }
  return "Unknown";
}

}  // namespace

void WebUiServer::attach(store::JobIndex* jobs, store::ApprovalOutboxIndex* outbox,
                          const config::DeviceConfigData* deviceConfig, uint16_t panelWidth, uint16_t panelHeight,
                          uint32_t wakeMillis, store::TaskIndex* plannerTasks) {
  jobs_ = jobs;
  outbox_ = outbox;
  deviceConfig_ = deviceConfig;
  panelWidth_ = panelWidth;
  panelHeight_ = panelHeight;
  wakeMillis_ = wakeMillis;
  plannerTasks_ = plannerTasks;
}

void WebUiServer::generateSessionSecrets() {
  randomPin(pin_);
  fwrand::randomHex(sessionToken_, 32);
}

bool WebUiServer::startStation(uint32_t timeoutMs) {
  stop();  // defensive: tear down any previous session first

  net::WifiManager wifi;
  if (!wifi.connect(timeoutMs)) {
    return false;  // no known network in range — caller (InboxUI) surfaces this
  }

  wifi.currentSsid().toCharArray(ssid_, sizeof(ssid_));  // joined network, shown on the status screen
  password_[0] = '\0';  // station mode: nothing to display here (it's the saved Wi-Fi password, not new)
  generateSessionSecrets();

  mode_ = WebUiMode::Station;
  beginCommon();
  return true;
}

bool WebUiServer::startHotspot() {
  stop();

  WiFi.mode(WIFI_AP);  // initializes the radio so macAddress() below is valid
  uint8_t mac[6];
  WiFi.macAddress(mac);
  const char* base =
      (deviceConfig_ != nullptr && deviceConfig_->deviceName[0] != '\0') ? deviceConfig_->deviceName : "Focusink";
  std::snprintf(ssid_, sizeof(ssid_), "%.30s-%02X%02X", base, mac[4], mac[5]);
  randomPassword(password_, sizeof(password_) - 1);
  generateSessionSecrets();

  net::WifiManager wifi;
  if (!wifi.startAccessPoint(ssid_, password_)) {
    mode_ = WebUiMode::Off;
    return false;
  }
  mode_ = WebUiMode::Hotspot;
  beginCommon();
  return true;
}

void WebUiServer::beginCommon() {
  if (!routesRegistered_) {
    server_.on("/", HTTP_GET, [this]() {
      markActivity();
      handleRoot();
    });
    server_.on("/login", HTTP_POST, [this]() {
      markActivity();
      handleLogin();
    });
    server_.on("/api/status", HTTP_GET, [this]() {
      markActivity();
      handleApiStatus();
    });
    server_.on("/api/diag", HTTP_GET, [this]() {
      markActivity();
      handleApiDiag();
    });
    server_.on("/planner", HTTP_GET, [this]() {
      markActivity();
      handlePlannerPage();
    });
    server_.on("/api/planner/tasks", HTTP_GET, [this]() {
      markActivity();
      handleApiPlannerTasksGet();
    });
    server_.on("/api/jobs", HTTP_GET, [this]() {
      markActivity();
      handleApiJobsGet();
    });
    server_.on("/api/jobs", HTTP_POST, [this]() {
      markActivity();
      handleApiJobsPost();
    });
    server_.on("/api/jobs/xtc", HTTP_GET, [this]() {
      markActivity();
      handleApiJobXtc();
    });
    server_.on("/xtc-decoder.wasm", HTTP_GET, [this]() {
      markActivity();
      handleXtcDecoderWasm();
    });
    server_.on("/xtc-decoder.js", HTTP_GET, [this]() {
      markActivity();
      handleXtcDecoderJs();
    });
    server_.on("/xtc-encoder.wasm", HTTP_GET, [this]() {
      markActivity();
      handleXtcEncoderWasm();
    });
    server_.on("/xtc-encoder.js", HTTP_GET, [this]() {
      markActivity();
      handleXtcEncoderJs();
    });
    server_.on(
        "/api/upload/xtc", HTTP_POST,
        [this]() {
          markActivity();
          handleUploadXtcComplete();
        },
        [this]() { handleUploadXtcData(); });
    server_.on(
        "/api/upload/original", HTTP_POST,
        [this]() {
          markActivity();
          handleUploadOriginalComplete();
        },
        [this]() { handleUploadOriginalData(); });
    server_.onNotFound([this]() {
      markActivity();
      handleNotFound();
    });
    routesRegistered_ = true;
  }
  server_.begin();
}

void WebUiServer::stop() {
  if (mode_ == WebUiMode::Off) return;
  server_.stop();
  net::WifiManager wifi;
  if (mode_ == WebUiMode::Hotspot) {
    wifi.stopAccessPoint();
  } else {
    wifi.disconnect();
  }
  mode_ = WebUiMode::Off;
  ssid_[0] = '\0';
  password_[0] = '\0';
  pin_[0] = '\0';
  sessionToken_[0] = '\0';
}

void WebUiServer::handleClient() {
  if (mode_ == WebUiMode::Off) return;
  server_.handleClient();
}

void WebUiServer::markActivity() { activityFlag_ = true; }

bool WebUiServer::consumeActivityFlag() {
  bool v = activityFlag_;
  activityFlag_ = false;
  return v;
}

String WebUiServer::address() const {
  if (mode_ == WebUiMode::Station) {
    net::WifiManager wifi;
    return wifi.currentIp();
  }
  if (mode_ == WebUiMode::Hotspot) {
    return WiFi.softAPIP().toString();
  }
  return String();
}

bool WebUiServer::requestHasValidSession() {
  if (sessionToken_[0] == '\0') return false;
  if (!server_.hasHeader("Cookie")) return false;
  String cookie = server_.header("Cookie");
  String needle = String("session=") + sessionToken_;
  return cookie.indexOf(needle) >= 0;
}

void WebUiServer::handleRoot() {
  if (!requestHasValidSession()) {
    sendGzip(200, "text/html", kLoginPageHtmlGz, kLoginPageHtmlGzLen);
    return;
  }
  sendGzip(200, "text/html", kJobListPageHtmlGz, kJobListPageHtmlGzLen);
}

// Planner page (docs/planner.md) -- same auth gate as the job list, no new
// login flow: an unauthenticated request is bounced to / (which shows the
// login page) rather than serving the page shell to a logged-out session.
void WebUiServer::handlePlannerPage() {
  if (!requestHasValidSession()) {
    server_.sendHeader("Location", "/");
    server_.send(303, "text/plain", "");
    return;
  }
  sendGzip(200, "text/html", kPlannerPageHtmlGz, kPlannerPageHtmlGzLen);
}

void WebUiServer::handleLogin() {
  // Deliberately not constant-time / not rate-limited — see
  // docs/security.md "On-device Web UI" for why that's an accepted
  // tradeoff here (same class of documented gap as the Pi's own APIs).
  if (server_.arg("pin") != String(pin_)) {
    server_.send(401, "text/html",
                 "<style>:root{--bg:#f7ecd1;--fg:#3a2115;--danger:#bd361e;--danger-bg:#f6ded6;"
                 "color-scheme:light dark}"
                 "@media (prefers-color-scheme:dark){:root{--bg:#1c1109;--fg:#f3e3c4;"
                 "--danger:#ff6a47;--danger-bg:#3a1810}}"
                 "body{background:var(--bg);color:var(--fg);font-family:\"Nunito Sans\",-apple-system,"
                 "BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif,"
                 "'Apple Color Emoji','Segoe UI Emoji';max-width:320px;margin:3rem auto;"
                 "padding:0 1rem}"
                 "a:focus-visible{outline:2px solid var(--danger);outline-offset:2px}</style>"
                 "<p style=\"color:var(--danger);background:var(--danger-bg);padding:.75rem;"
                 "border-radius:16px\">Wrong PIN. <a href=\"/\">Try again</a>.</p>");
    return;
  }
  String cookie = String("session=") + sessionToken_ + "; Path=/; HttpOnly";
  server_.sendHeader("Set-Cookie", cookie);
  server_.sendHeader("Location", "/");
  server_.send(303, "text/plain", "");
}

void WebUiServer::handleApiStatus() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  JsonDocument doc;
  doc["device_name"] =
      (deviceConfig_ != nullptr && deviceConfig_->deviceName[0]) ? deviceConfig_->deviceName : "Focusink";
  doc["mode"] = mode_ == WebUiMode::Hotspot ? "hotspot" : "station";
  size_t total = jobs_ != nullptr ? jobs_->count() : 0;
  size_t unread = 0;
  for (size_t i = 0; jobs_ != nullptr && i < total; i++) {
    if (jobs_->at(i).status == store::JobStatus::Downloaded) unread++;
  }
  doc["job_count"] = total;
  doc["unread_count"] = unread;
  // Local SD-card state, meaningful in both modes — not gated on
  // connectivity the way pi_admin_base_url/wifi_rssi below are.
  doc["outbox_pending"] = outbox_ != nullptr ? outbox_->countUnsynced() : 0;
  if (deviceConfig_ != nullptr) {
    doc["device_id"] = deviceConfig_->deviceId;
  }
  // Only in station mode: hotspot mode's phone has no network path to the
  // Pi at all (see docs/architecture.md "On-device Web UI full-document
  // preview"), so there's nothing useful to link to there even if this
  // device happens to have been paired with the admin console enabled.
  // Same reasoning for wifi_rssi — "signal to the AP" is meaningless when
  // this device is itself the AP.
  if (mode_ == WebUiMode::Station) {
    if (deviceConfig_ != nullptr && deviceConfig_->hasAdminConsole) {
      doc["pi_admin_base_url"] = deviceConfig_->piAdminBaseUrl;
    }
    net::WifiManager wifi;
    doc["wifi_rssi"] = wifi.rssi();
  }

  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

// Split from handleApiStatus() rather than folded in: /api/status is polled
// every 20s by the job-list page, and device_name/id/panel size/uptime
// don't need that freshness — bloating the hot-polled payload with static
// data would waste bytes on the hotspot AP's own limited bandwidth (the
// same reasoning firmware/src/ui/pages/generate_pages_header.py's docstring
// gives for gzip-embedding the pages themselves). The job-list page fetches
// this once, lazily, only if the diagnostics panel is opened.
void WebUiServer::handleApiDiag() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  JsonDocument doc;
  doc["device_name"] =
      (deviceConfig_ != nullptr && deviceConfig_->deviceName[0]) ? deviceConfig_->deviceName : "Focusink";
  doc["device_id"] = deviceConfig_ != nullptr ? deviceConfig_->deviceId : "";
  doc["panel_width"] = panelWidth_;
  doc["panel_height"] = panelHeight_;
  doc["uptime_seconds"] = (millis() - wakeMillis_) / 1000;
  // sdUsedBytes() scans the FAT and is cached with a 20s TTL by SDCardManager
  // itself (see SDCardManager.h) -- cheap enough to call on every lazy diag
  // fetch without adding our own caching layer on top.
  const uint64_t sdTotal = SdMan.sdTotalBytes();
  const uint64_t sdUsed = SdMan.sdUsedBytes();
  doc["sd_total_bytes"] = sdTotal;
  doc["sd_free_bytes"] = sdTotal >= sdUsed ? sdTotal - sdUsed : 0;

  // X4 is an ADC-backed board (BoardConfig::XTEINK_X4 has no charge-status
  // pin), so percentage/millivolts are the only fields this hardware can
  // report -- chargingKnown/externalPowerKnown always come back false here,
  // not a bug in this call. Omit rather than send a misleading always-false
  // charging flag.
  const BatteryMonitor::Status battery = BatteryMonitor().readStatus();
  if (battery.percentageKnown) doc["battery_percent"] = battery.percentage;
  if (battery.millivoltsKnown) doc["battery_millivolts"] = battery.millivolts;

  doc["heap_free_bytes"] = freeink::MemoryManager::instance().freeBytes();

  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

// Merged tasks + calendar next-event for the Planner page -- reads
// plannerTasks_ and the calendar module's singleton cache directly
// on-device, no round-trip to the Pi (docs/architecture.md's "Planner
// page" section). The `date` query param is accepted for shape parity
// with the Pi's own GET .../planner/tasks?date=... contract
// (docs/protocol.md §1.8), but not actually filtered on here: the
// on-device store::TaskIndex has no per-task date field of its own --
// it's inherently a single day's snapshot (whatever the last sync
// pushed down), not a multi-day store -- so this always returns that
// whole snapshot regardless of the param's value.
void WebUiServer::handleApiPlannerTasksGet() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc["tasks"].to<JsonArray>();
  for (size_t i = 0; plannerTasks_ != nullptr && i < plannerTasks_->count(); i++) {
    const store::TaskEntry& t = plannerTasks_->at(i);
    JsonObject j = arr.add<JsonObject>();
    j["id"] = t.id;
    j["title"] = t.title;
    j["category"] = store::categoryName(t.category);
    j["start_time"] = t.startTime;
    j["end_time"] = t.endTime;
    j["done"] = t.done;
    j["source"] = "task";
  }

  const config::NextEventInfo& event = config::CalendarCache::instance().data();
  if (event.hasEvent) {
    char hm[6];
    ui::formatUtcHm(event.start, hm, sizeof(hm));  // UTC -- see that function's own caveat

    JsonObject j = arr.add<JsonObject>();
    j["id"] = "calendar";
    j["title"] = event.title;
    j["category"] = "Other";
    j["start_time"] = hm;
    j["end_time"] = hm;
    j["done"] = false;
    j["source"] = "calendar";
  }

  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

void WebUiServer::handleApiJobsGet() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc["jobs"].to<JsonArray>();
  for (size_t i = 0; jobs_ != nullptr && i < jobs_->count(); i++) {
    const store::JobEntry& e = jobs_->at(i);
    if (e.status == store::JobStatus::ApprovedDelete) continue;  // hidden, same as the physical Inbox screen
    JsonObject j = arr.add<JsonObject>();
    j["job_id"] = e.jobId;
    j["title"] = e.title;
    j["page_count"] = e.pageCount;
    j["status"] = jobStatusLabel(e.status);
    j["xtc_bytes"] = e.xtcBytes;
    j["xtc_sha256"] = e.xtcSha256;
    j["created_at"] = e.createdAt;
    j["pending_approval"] = outbox_ != nullptr && outbox_->hasPendingForJob(e.jobId);
    j["original_pending"] = e.originalPending;
  }

  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

void WebUiServer::handleApiJobsPost() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (jobs_ == nullptr || outbox_ == nullptr) {
    server_.send(500, "application/json", "{\"error\":\"not ready\"}");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, server_.arg("plain"))) {
    server_.send(400, "application/json", "{\"error\":\"invalid body\"}");
    return;
  }
  const char* jobId = doc["job_id"] | "";
  const char* actionStr = doc["action"] | "";
  store::ApprovalAction action;
  if (jobId[0] == '\0' || !store::parseApprovalAction(actionStr, action)) {
    server_.send(400, "application/json", "{\"error\":\"invalid job_id/action\"}");
    return;
  }

  char approvalId[store::kApprovalIdLen + 1];
  fwrand::randomHex(approvalId, store::kApprovalIdLen);
  uint32_t createdAt = static_cast<uint32_t>(std::time(nullptr));

  store::EnqueueResult result = store::enqueueApproval(*jobs_, *outbox_, jobId, action, approvalId, createdAt);
  if (result == store::EnqueueResult::Ok) {
    store::saveApprovalOutbox(*outbox_);
    store::saveJobIndex(*jobs_);
    server_.send(200, "application/json", "{\"status\":\"ok\"}");
    return;
  }

  int code = 409;
  const char* err = "already_pending";
  if (result == store::EnqueueResult::UnknownJob) {
    code = 404;
    err = "unknown_job";
  } else if (result == store::EnqueueResult::OutboxFull) {
    code = 409;
    err = "outbox_full";
  }

  JsonDocument errDoc;
  errDoc["error"] = err;
  String out;
  serializeJson(errDoc, out);
  server_.send(code, "application/json", out);
}

void WebUiServer::handleApiJobXtc() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (jobs_ == nullptr) {
    server_.send(500, "application/json", "{\"error\":\"not ready\"}");
    return;
  }

  String jobId = server_.arg("job_id");
  const store::JobEntry* entry = jobs_->find(jobId.c_str());
  if (entry == nullptr) {
    server_.send(404, "application/json", "{\"error\":\"unknown_job\"}");
    return;
  }

  FsFile file = SdMan.open(entry->xtcPath, O_RDONLY);
  if (!file) {
    server_.send(500, "application/json", "{\"error\":\"io_error\"}");
    return;
  }

  // Streamed straight from SD in bounded chunks (never a whole-file
  // buffer), same reasoning as xtc::XtcReader.cpp's own bulk-copy path —
  // the client (tools/xtc-wasm/xtc_decoder.cpp, via ui/pages/joblist.html's
  // preview button) does its own parsing/bounds-checking on these raw
  // bytes, same as any other untrusted input.
  server_.setContentLength(entry->xtcBytes);
  server_.send(200, "application/octet-stream", "");

  uint8_t buf[kXtcStreamChunkBytes];
  uint32_t remaining = entry->xtcBytes;
  while (remaining > 0) {
    size_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
    int n = file.read(buf, toRead);
    if (n <= 0) break;  // io error mid-stream; client sees a short body and treats decode as failed
    server_.sendContent(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    remaining -= static_cast<uint32_t>(n);
  }
  file.close();
}

void WebUiServer::handleXtcDecoderWasm() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sendGzip(200, "application/wasm", kXtcDecoderWasmGz, kXtcDecoderWasmGzLen);
}

void WebUiServer::handleXtcDecoderJs() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sendGzip(200, "application/javascript", kXtcDecoderJsGz, kXtcDecoderJsGzLen);
}

void WebUiServer::handleXtcEncoderWasm() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sendGzip(200, "application/wasm", kXtcEncoderWasmGz, kXtcEncoderWasmGzLen);
}

void WebUiServer::handleXtcEncoderJs() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sendGzip(200, "application/javascript", kXtcEncoderJsGz, kXtcEncoderJsGzLen);
}

// -- POST /api/upload/xtc: the job-list page's "Upload" button hands us an
// already-client-side-encoded single-page XTC file (see
// tools/xtc-wasm/xtc_encoder.cpp), which becomes a normal JobEntry as soon
// as it's written -- readable/approvable immediately, exactly like a
// Pi-synced job. Requires a multipart/form-data body with one file field
// (WebServer's upload-handler mechanism only fires for multipart bodies —
// see WebServer.h/Parsing.cpp); title comes from a query-string arg since
// it isn't part of the file's own bytes.
//
// The upload-handler callback (this method) runs repeatedly as the
// request body streams in, well before the completion handler
// (handleUploadXtcComplete()) — that's why the outcome is staged into
// member state rather than being decided here.
void WebUiServer::handleUploadXtcData() {
  HTTPUpload& upload = server_.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadXtcAuthorized_ = false;
    uploadXtcTooLarge_ = false;
    uploadXtcError_ = nullptr;
    uploadXtcBytes_ = 0;

    if (!requestHasValidSession()) {
      uploadXtcError_ = "unauthorized";
      return;
    }
    if (jobs_ == nullptr) {
      uploadXtcError_ = "not_ready";
      return;
    }
    if (jobs_->full()) {
      uploadXtcError_ = "inbox_full";
      return;
    }

    fwrand::randomHex(uploadXtcJobId_, store::kJobIdLen);
    std::snprintf(uploadXtcPath_, sizeof(uploadXtcPath_), "/inbox/%s.xtc", uploadXtcJobId_);
    SdMan.ensureDirectoryExists("/inbox");
    uploadXtcFile_ = SdMan.open(uploadXtcPath_, O_WRONLY | O_CREAT | O_TRUNC);
    if (!uploadXtcFile_) {
      uploadXtcError_ = "io_error";
      return;
    }
    uploadXtcAuthorized_ = true;
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadXtcAuthorized_ || !uploadXtcFile_) return;
    if (uploadXtcBytes_ + upload.currentSize > kMaxXtcUploadBytes) {
      uploadXtcTooLarge_ = true;
      return;
    }
    if (uploadXtcTooLarge_) return;  // already over budget; keep draining the request without writing more
    uploadXtcFile_.write(upload.buf, upload.currentSize);
    uploadXtcBytes_ += upload.currentSize;
    return;
  }

  // UPLOAD_FILE_END or UPLOAD_FILE_ABORTED
  if (uploadXtcFile_) uploadXtcFile_.close();
}

void WebUiServer::handleUploadXtcComplete() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (!uploadXtcAuthorized_) {
    SdMan.remove(uploadXtcPath_);
    JsonDocument errDoc;
    errDoc["error"] = uploadXtcError_ != nullptr ? uploadXtcError_ : "upload_failed";
    String out;
    serializeJson(errDoc, out);
    server_.send(uploadXtcError_ != nullptr && std::strcmp(uploadXtcError_, "inbox_full") == 0 ? 409 : 400,
                 "application/json", out);
    return;
  }
  if (uploadXtcTooLarge_) {
    SdMan.remove(uploadXtcPath_);
    server_.send(413, "application/json", "{\"error\":\"too_large\"}");
    return;
  }
  if (uploadXtcBytes_ == 0) {
    SdMan.remove(uploadXtcPath_);
    server_.send(400, "application/json", "{\"error\":\"empty_upload\"}");
    return;
  }

  // Validate the file we just wrote with the same reader firmware uses to
  // render pages, rather than trusting the client's claim that this is a
  // well-formed XTC file -- derives page_count from the file itself.
  xtc::XtcReader reader;
  if (!reader.open(uploadXtcPath_) || reader.pageCount() == 0) {
    reader.close();
    SdMan.remove(uploadXtcPath_);
    server_.send(400, "application/json", "{\"error\":\"invalid_xtc\"}");
    return;
  }
  uint16_t pageCount = reader.pageCount();
  reader.close();

  store::JobEntry entry;
  std::strncpy(entry.jobId, uploadXtcJobId_, sizeof(entry.jobId) - 1);
  String title = server_.arg("title");
  if (title.isEmpty()) title = "Untitled";
  title.toCharArray(entry.title, sizeof(entry.title));
  std::strncpy(entry.xtcPath, uploadXtcPath_, sizeof(entry.xtcPath) - 1);
  entry.xtcBytes = uploadXtcBytes_;
  entry.pageCount = pageCount;
  entry.createdAt = static_cast<uint32_t>(std::time(nullptr));
  entry.status = store::JobStatus::Downloaded;

  if (!jobs_->upsert(entry)) {
    SdMan.remove(uploadXtcPath_);
    server_.send(409, "application/json", "{\"error\":\"inbox_full\"}");
    return;
  }
  store::saveJobIndex(*jobs_);

  JsonDocument doc;
  doc["job_id"] = entry.jobId;
  String out;
  serializeJson(doc, out);
  server_.send(200, "application/json", out);
}

// -- POST /api/upload/original: the second step of the same "Upload" flow
// -- hands us the ORIGINAL (undithered, full color/resolution) image bytes
// for a job /api/upload/xtc already created, so the Pi can later run them
// through its normal conversion pipeline for a real print (the XTC file
// alone is a lossy 1bpp e-paper rendering, not print stock). Marks the job
// originalPending so SyncManager::uploadPendingOriginals() picks it up on
// the next real sync -- see docs/architecture.md's direct-upload section.
void WebUiServer::handleUploadOriginalData() {
  HTTPUpload& upload = server_.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadOriginalAuthorized_ = false;
    uploadOriginalTooLarge_ = false;
    uploadOriginalError_ = nullptr;
    uploadOriginalBytes_ = 0;

    if (!requestHasValidSession()) {
      uploadOriginalError_ = "unauthorized";
      return;
    }
    if (jobs_ == nullptr) {
      uploadOriginalError_ = "not_ready";
      return;
    }

    String jobId = server_.arg("job_id");
    const store::JobEntry* existing = jobs_->find(jobId.c_str());
    if (existing == nullptr) {
      uploadOriginalError_ = "unknown_job";
      return;
    }

    String mime = server_.arg("mime");
    const char* ext = nullptr;
    if (mime == "image/jpeg") {
      ext = "jpg";
    } else if (mime == "image/png") {
      ext = "png";
    } else {
      uploadOriginalError_ = "unsupported_mime";
      return;
    }

    std::strncpy(uploadOriginalJobId_, jobId.c_str(), sizeof(uploadOriginalJobId_) - 1);
    mime.toCharArray(uploadOriginalMime_, sizeof(uploadOriginalMime_));
    std::snprintf(uploadOriginalPath_, sizeof(uploadOriginalPath_), "/inbox/%s_orig.%s", uploadOriginalJobId_, ext);
    SdMan.ensureDirectoryExists("/inbox");
    uploadOriginalFile_ = SdMan.open(uploadOriginalPath_, O_WRONLY | O_CREAT | O_TRUNC);
    if (!uploadOriginalFile_) {
      uploadOriginalError_ = "io_error";
      return;
    }
    uploadOriginalAuthorized_ = true;
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadOriginalAuthorized_ || !uploadOriginalFile_) return;
    if (uploadOriginalBytes_ + upload.currentSize > kMaxOriginalUploadBytes) {
      uploadOriginalTooLarge_ = true;
      return;
    }
    if (uploadOriginalTooLarge_) return;
    uploadOriginalFile_.write(upload.buf, upload.currentSize);
    uploadOriginalBytes_ += upload.currentSize;
    return;
  }

  if (uploadOriginalFile_) uploadOriginalFile_.close();
}

void WebUiServer::handleUploadOriginalComplete() {
  if (!requestHasValidSession()) {
    server_.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (!uploadOriginalAuthorized_) {
    if (uploadOriginalPath_[0] != '\0') SdMan.remove(uploadOriginalPath_);
    JsonDocument errDoc;
    errDoc["error"] = uploadOriginalError_ != nullptr ? uploadOriginalError_ : "upload_failed";
    String out;
    serializeJson(errDoc, out);
    int code = 400;
    if (uploadOriginalError_ != nullptr && std::strcmp(uploadOriginalError_, "unknown_job") == 0) code = 404;
    server_.send(code, "application/json", out);
    return;
  }
  if (uploadOriginalTooLarge_) {
    SdMan.remove(uploadOriginalPath_);
    server_.send(413, "application/json", "{\"error\":\"too_large\"}");
    return;
  }
  if (uploadOriginalBytes_ == 0) {
    SdMan.remove(uploadOriginalPath_);
    server_.send(400, "application/json", "{\"error\":\"empty_upload\"}");
    return;
  }

  store::JobEntry* entry = jobs_->find(uploadOriginalJobId_);
  if (entry == nullptr) {
    // The job was deleted while this upload was in flight -- the original
    // has nowhere to attach to any more.
    SdMan.remove(uploadOriginalPath_);
    server_.send(404, "application/json", "{\"error\":\"unknown_job\"}");
    return;
  }

  entry->originalPending = true;
  std::strncpy(entry->originalPath, uploadOriginalPath_, sizeof(entry->originalPath) - 1);
  std::strncpy(entry->originalMime, uploadOriginalMime_, sizeof(entry->originalMime) - 1);
  entry->originalBytes = uploadOriginalBytes_;
  store::saveJobIndex(*jobs_);

  server_.send(200, "application/json", "{\"status\":\"ok\"}");
}

void WebUiServer::sendGzip(int code, const char* contentType, const unsigned char* data, size_t len) {
  server_.sendHeader("Content-Encoding", "gzip");
  server_.setContentLength(len);
  server_.send(code, contentType, "");
  server_.sendContent(reinterpret_cast<const char*>(data), len);
}

void WebUiServer::handleNotFound() { server_.send(404, "text/plain", "Not found"); }

}  // namespace ui
