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

#include "net/WifiManager.h"
#include "ui/PagesData.h"
#include "ui/XtcDecoderWasmData.h"
#include "util/Random.h"

namespace ui {

namespace {

// PIN/password generators are distinct formats (decimal digits;
// screen-readable charset) from fwrand::randomHex() (util/Random.h,
// shared with ui/InboxUI.cpp's approval-id generation), so they stay
// local here rather than being folded into that shared helper.

// Matches xtc::XtcReader.cpp's own kBulkChunkBytes — bounded RAM regardless
// of job size, no full-file buffer.
constexpr size_t kXtcStreamChunkBytes = 2048;

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
                          uint32_t wakeMillis) {
  jobs_ = jobs;
  outbox_ = outbox;
  deviceConfig_ = deviceConfig;
  panelWidth_ = panelWidth;
  panelHeight_ = panelHeight;
  wakeMillis_ = wakeMillis;
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
      (deviceConfig_ != nullptr && deviceConfig_->deviceName[0] != '\0') ? deviceConfig_->deviceName : "Xteink X4";
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
      (deviceConfig_ != nullptr && deviceConfig_->deviceName[0]) ? deviceConfig_->deviceName : "Xteink X4";
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
      (deviceConfig_ != nullptr && deviceConfig_->deviceName[0]) ? deviceConfig_->deviceName : "Xteink X4";
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

void WebUiServer::sendGzip(int code, const char* contentType, const unsigned char* data, size_t len) {
  server_.sendHeader("Content-Encoding", "gzip");
  server_.setContentLength(len);
  server_.send(code, contentType, "");
  server_.sendContent(reinterpret_cast<const char*>(data), len);
}

void WebUiServer::handleNotFound() { server_.send(404, "text/plain", "Not found"); }

}  // namespace ui
