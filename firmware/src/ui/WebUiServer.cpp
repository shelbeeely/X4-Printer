#include "ui/WebUiServer.h"

#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "net/WifiManager.h"
#include "util/Random.h"

namespace ui {

namespace {

// PIN/password generators are distinct formats (decimal digits;
// screen-readable charset) from fwrand::randomHex() (util/Random.h,
// shared with ui/InboxUI.cpp's approval-id generation), so they stay
// local here rather than being folded into that shared helper.

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

constexpr const char* kLoginPageHtml =
    R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>X4 Print Inbox</title>
<style>
:root{
  --bg:#ffffff;--fg:#1a1a1a;--muted:#5a6270;--accent:#2563eb;--accent-fg:#ffffff;
  --card-bg:#f6f7f9;--border:#e0e2e7;--danger:#b3261e;--danger-bg:#fdecea;
  color-scheme:light dark;
}
@media (prefers-color-scheme: dark) {
  :root{
    --bg:#14161a;--fg:#eef0f3;--muted:#9aa2b1;--accent:#6ea8fe;--accent-fg:#ffffff;
    --card-bg:#1c1f26;--border:#2c3038;--danger:#ff8478;--danger-bg:#3a1a18;
  }
}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";max-width:320px;margin:3rem auto;padding:0 1rem;background:var(--bg);color:var(--fg)}
header{border-bottom:1px solid var(--border);padding-bottom:.5rem;margin-bottom:1rem}
header h1{margin:0;font-size:1.3rem;font-weight:700}
input{font-size:1.5rem;width:100%;padding:.5rem;text-align:center;letter-spacing:.3em;box-sizing:border-box;background:var(--card-bg);color:var(--fg);border:1px solid var(--border);border-radius:8px}
button{width:100%;padding:.75rem;font-size:1.1rem;margin-top:1rem;background:var(--accent);border:1px solid var(--accent);color:var(--accent-fg);border-radius:8px;font-weight:600}
</style></head>
<body><header><h1>X4 Print Inbox</h1></header><p>Enter the PIN shown on the device screen.</p>
<form method="POST" action="/login">
<input name="pin" inputmode="numeric" pattern="[0-9]*" maxlength="6" autofocus>
<button type="submit">Unlock</button></form></body></html>)HTML";

constexpr const char* kJobListPageHtml =
    R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>X4 Print Inbox</title>
<style>
:root{
  --bg:#ffffff;--fg:#1a1a1a;--muted:#5a6270;--accent:#2563eb;--accent-fg:#ffffff;
  --card-bg:#f6f7f9;--border:#e0e2e7;--danger:#b3261e;--danger-bg:#fdecea;
  color-scheme:light dark;
}
@media (prefers-color-scheme: dark) {
  :root{
    --bg:#14161a;--fg:#eef0f3;--muted:#9aa2b1;--accent:#6ea8fe;--accent-fg:#ffffff;
    --card-bg:#1c1f26;--border:#2c3038;--danger:#ff8478;--danger-bg:#3a1a18;
  }
}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji";max-width:480px;margin:1rem auto;padding:0 1rem;background:var(--bg);color:var(--fg)}
header{border-bottom:1px solid var(--border);padding-bottom:.5rem;margin-bottom:.75rem}
header h1{margin:0;font-size:1.3rem;font-weight:700}
.job{background:var(--card-bg);border:1px solid var(--border);border-radius:10px;padding:.75rem;margin:.5rem 0;box-shadow:0 1px 2px rgba(0,0,0,.04)}
.job h3{margin:0 0 .25rem;font-size:1rem}
.job button{margin-right:.4rem;padding:.4rem .7rem;border-radius:8px;font-weight:600;background:var(--card-bg);border:1px solid var(--border);color:var(--fg)}
.job button:first-of-type{background:var(--accent);border:1px solid var(--accent);color:var(--accent-fg)}
#status{color:var(--muted);font-size:.85rem}
</style></head><body>
<header><h1>Print Inbox</h1></header>
<p id="status">Loading...</p>
<div id="jobs"></div>
<script>
async function refreshStatus() {
  const r = await fetch('/api/status');
  if (!r.ok) return;
  const s = await r.json();
  document.getElementById('status').textContent =
    s.device_name + ' - ' + s.unread_count + ' unread of ' + s.job_count;
}
async function refreshJobs() {
  const r = await fetch('/api/jobs');
  if (!r.ok) return;
  const data = await r.json();
  const el = document.getElementById('jobs');
  el.innerHTML = '';
  if (data.jobs.length === 0) { el.innerHTML = '<p>Inbox empty.</p>'; return; }
  for (const j of data.jobs) {
    const div = document.createElement('div');
    div.className = 'job';
    const h = document.createElement('h3');
    h.textContent = j.title + ' (' + j.page_count + ' pg)';
    div.appendChild(h);
    if (j.pending_approval) {
      const p = document.createElement('p');
      p.textContent = 'Action queued, syncing...';
      div.appendChild(p);
    } else {
      for (const pair of [['Print','print'], ['Keep','keep'], ['Delete','delete']]) {
        const b = document.createElement('button');
        b.textContent = pair[0];
        b.onclick = () => act(j.job_id, pair[1]);
        div.appendChild(b);
      }
    }
    el.appendChild(div);
  }
}
async function act(jobId, action) {
  await fetch('/api/jobs', {method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({job_id: jobId, action: action})});
  refreshJobs();
}
refreshStatus();
refreshJobs();
setInterval(() => { refreshStatus(); refreshJobs(); }, 20000);
</script></body></html>)HTML";

}  // namespace

void WebUiServer::attach(store::JobIndex* jobs, store::ApprovalOutboxIndex* outbox,
                          const config::DeviceConfigData* deviceConfig) {
  jobs_ = jobs;
  outbox_ = outbox;
  deviceConfig_ = deviceConfig;
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
    server_.on("/api/jobs", HTTP_GET, [this]() {
      markActivity();
      handleApiJobsGet();
    });
    server_.on("/api/jobs", HTTP_POST, [this]() {
      markActivity();
      handleApiJobsPost();
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
    server_.send(200, "text/html", kLoginPageHtml);
    return;
  }
  server_.send(200, "text/html", kJobListPageHtml);
}

void WebUiServer::handleLogin() {
  // Deliberately not constant-time / not rate-limited — see
  // docs/security.md "On-device Web UI" for why that's an accepted
  // tradeoff here (same class of documented gap as the Pi's own APIs).
  if (server_.arg("pin") != String(pin_)) {
    server_.send(401, "text/html",
                 "<style>:root{--bg:#fff;--fg:#1a1a1a;--danger:#b3261e;--danger-bg:#fdecea;"
                 "color-scheme:light dark}"
                 "@media (prefers-color-scheme:dark){:root{--bg:#14161a;--fg:#eef0f3;"
                 "--danger:#ff8478;--danger-bg:#3a1a18}}"
                 "body{background:var(--bg);color:var(--fg);font-family:-apple-system,"
                 "BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif,"
                 "'Apple Color Emoji','Segoe UI Emoji';max-width:320px;margin:3rem auto;"
                 "padding:0 1rem}</style>"
                 "<p style=\"color:var(--danger);background:var(--danger-bg);padding:.75rem;"
                 "border-radius:8px\">Wrong PIN. <a href=\"/\">Try again</a>.</p>");
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
    j["status"] = static_cast<uint8_t>(e.status);
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

void WebUiServer::handleNotFound() { server_.send(404, "text/plain", "Not found"); }

}  // namespace ui
