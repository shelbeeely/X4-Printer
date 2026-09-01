#include "net/SyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SDCardManager.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

namespace net {

namespace {

const char* kPiCaPath = "/system/pi_ca.pem";
const char* kRelayCaPath = "/system/relay_ca.pem";

// Loads a PEM CA certificate from SD into `out` (caller-owned buffer).
// Returns false if the file doesn't exist or is too large — callers fall
// back to setInsecure() in that case (see header note in
// docs/security.md about the relay's optional pinning).
bool loadCaCert(const char* path, String& out) {
  if (!SdMan.exists(path)) return false;
  out = SdMan.readFile(path);
  return !out.isEmpty();
}

bool configureClientForEndpoint(WiFiClientSecure& client, Endpoint endpoint) {
  static String piCa;
  static String relayCa;
  static bool piCaLoaded = false;
  static bool relayCaLoaded = false;

  if (endpoint == Endpoint::Pi) {
    if (!piCaLoaded) {
      piCaLoaded = loadCaCert(kPiCaPath, piCa);
    }
    if (piCaLoaded) {
      client.setCACert(piCa.c_str());
      return true;
    }
    // No pinned cert provisioned yet (device not fully paired) — refuse to
    // fall back to insecure for the Pi endpoint, since that's the endpoint
    // carrying the device's real bearer token on every request.
    return false;
  }

  // Relay: prefer a provisioned CA (see docs/security.md), otherwise fall
  // back to the platform's default trust store for a public-CA-issued
  // relay certificate (e.g. Let's Encrypt) — WiFiClientSecure with no
  // setCACert/setInsecure call uses ESP-IDF's default verification, which
  // requires setCACertBundle() to have real roots; production deployments
  // should provision /system/relay_ca.pem explicitly (pair_device.py does
  // this automatically when FOCUSINK_RELAY_URL is configured — see
  // docs/relay.md).
  if (!relayCaLoaded) {
    relayCaLoaded = loadCaCert(kRelayCaPath, relayCa);
  }
  if (relayCaLoaded) {
    client.setCACert(relayCa.c_str());
  } else {
    client.setInsecure();
  }
  return true;
}

String buildAuthHeader(const char* token) { return String("Bearer ") + token; }

void sha256HexOf(const uint8_t digest[32], char out[65]) {
  static const char* hexDigits = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[i * 2] = hexDigits[(digest[i] >> 4) & 0xF];
    out[i * 2 + 1] = hexDigits[digest[i] & 0xF];
  }
  out[64] = '\0';
}

}  // namespace

bool SyncClient::statusCheck(Endpoint endpoint) {
  if (endpoint == Endpoint::Pi && !piConfigured()) return false;
  if (endpoint == Endpoint::Relay && !relayConfigured()) return false;

  WiFiClientSecure client;
  if (!configureClientForEndpoint(client, endpoint)) return false;

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  String url;
  if (endpoint == Endpoint::Pi) {
    url = String(cfg_.piBaseUrl) + "/devices/" + cfg_.deviceId + "/status";
  } else {
    url = String(cfg_.relayBaseUrl) + "/accounts/" + cfg_.relayAccountId + "/approvals/pending";
  }

  if (!http.begin(client, url)) return false;
  const char* token = (endpoint == Endpoint::Pi) ? cfg_.deviceToken : cfg_.relayAccountToken;
  http.addHeader("Authorization", buildAuthHeader(token));
  if (endpoint == Endpoint::Pi) http.addHeader("X-Device-Id", cfg_.deviceId);

  int code = http.GET();
  http.end();
  return code == 200;
}

int SyncClient::fetchPendingJobs(JobManifest* out, size_t maxCount) {
  if (!piConfigured()) return -1;

  WiFiClientSecure client;
  if (!configureClientForEndpoint(client, Endpoint::Pi)) return -1;

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  String url = String(cfg_.piBaseUrl) + "/devices/" + cfg_.deviceId + "/jobs?status=pending";
  if (!http.begin(client, url)) return -1;
  http.addHeader("Authorization", buildAuthHeader(cfg_.deviceToken));
  http.addHeader("X-Device-Id", cfg_.deviceId);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return -1;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return -1;

  JsonArrayConst jobs = doc["jobs"].as<JsonArrayConst>();
  size_t n = 0;
  for (JsonObjectConst job : jobs) {
    if (n >= maxCount) break;
    JobManifest& m = out[n];
    std::strncpy(m.jobId, job["job_id"] | "", sizeof(m.jobId) - 1);
    std::strncpy(m.title, job["title"] | "", sizeof(m.title) - 1);
    m.createdAt = job["created_at"] | 0;
    m.xtcBytes = job["xtc_bytes"] | 0;
    std::strncpy(m.xtcSha256, job["xtc_sha256"] | "", sizeof(m.xtcSha256) - 1);
    m.pageCount = job["page_count"] | 0;
    m.landscapeXtcBytes = job["landscape_xtc_bytes"] | 0;
    std::strncpy(m.landscapeXtcSha256, job["landscape_xtc_sha256"] | "", sizeof(m.landscapeXtcSha256) - 1);
    m.landscapePageCount = job["landscape_page_count"] | 0;
    n++;
  }
  return static_cast<int>(n);
}

bool SyncClient::fetchDeviceConfig(DeviceConfigManifest& out) {
  if (!piConfigured()) return false;

  WiFiClientSecure client;
  if (!configureClientForEndpoint(client, Endpoint::Pi)) return false;

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  String url = String(cfg_.piBaseUrl) + "/devices/" + cfg_.deviceId + "/config";
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", buildAuthHeader(cfg_.deviceToken));
  http.addHeader("X-Device-Id", cfg_.deviceId);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;

  DeviceConfigManifest parsed;

  JsonArrayConst calendars = doc["calendars"].as<JsonArrayConst>();
  for (JsonObjectConst cal : calendars) {
    if (parsed.calendarCount >= config::kMaxCalendars) break;
    const char* url2 = cal["url"] | "";
    if (url2[0] == '\0') continue;
    config::CalendarFeed& feed = parsed.calendars[parsed.calendarCount];
    std::strncpy(feed.url, url2, sizeof(feed.url) - 1);
    std::strncpy(feed.label, cal["label"] | "", sizeof(feed.label) - 1);
    parsed.calendarCount++;
  }

  JsonArrayConst wifiNetworks = doc["wifi_networks"].as<JsonArrayConst>();
  for (JsonObjectConst net : wifiNetworks) {
    if (parsed.wifiCount >= config::kMaxWifiNetworks) break;
    const char* ssid = net["ssid"] | "";
    if (ssid[0] == '\0') continue;
    config::WifiCredential& cred = parsed.wifiNetworks[parsed.wifiCount];
    std::strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
    std::strncpy(cred.password, net["password"] | "", sizeof(cred.password) - 1);
    parsed.wifiCount++;
  }

  out = parsed;
  return true;
}

bool SyncClient::downloadJobToSd(const char* jobId, const char* destPath, const char* expectedSha256Hex,
                                  uint32_t expectedBytes, const char* variant) {
  if (!piConfigured()) return false;

  WiFiClientSecure client;
  if (!configureClientForEndpoint(client, Endpoint::Pi)) return false;

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  String url = String(cfg_.piBaseUrl) + "/jobs/" + jobId + "/xtc";
  if (variant != nullptr) url += String("?variant=") + variant;
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", buildAuthHeader(cfg_.deviceToken));
  http.addHeader("X-Device-Id", cfg_.deviceId);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  int contentLength = http.getSize();
  if (contentLength > 0 && static_cast<uint32_t>(contentLength) != expectedBytes) {
    http.end();
    return false;
  }

  String tmpPath = String(destPath) + ".part";
  FsFile file = SdMan.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256 (not SHA-224)

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[kStreamChunkBytes];
  uint32_t totalRead = 0;
  bool ioError = false;
  uint32_t lastByteAt = millis();

  while (http.connected() && totalRead < expectedBytes) {
    size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastByteAt > kHttpTimeoutMs) {
        ioError = true;
        break;
      }
      delay(10);
      continue;
    }
    size_t toRead = available > sizeof(buf) ? sizeof(buf) : available;
    int n = stream->read(buf, toRead);
    if (n <= 0) {
      ioError = true;
      break;
    }
    lastByteAt = millis();

    if (file.write(buf, n) != static_cast<size_t>(n)) {
      ioError = true;
      break;
    }
    mbedtls_sha256_update(&sha, buf, n);
    totalRead += n;
  }

  file.close();
  http.end();

  if (ioError || totalRead != expectedBytes) {
    mbedtls_sha256_free(&sha);
    SdMan.remove(tmpPath.c_str());
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  char computedHex[65];
  sha256HexOf(digest, computedHex);

  if (std::strncmp(computedHex, expectedSha256Hex, 64) != 0) {
    SdMan.remove(tmpPath.c_str());
    return false;
  }

  // Only now — after full verification — does the file become the real
  // job file, matching docs/architecture.md wake step 5 ("on mismatch,
  // delete the partial file and leave the job pending for the next wake").
  if (SdMan.exists(destPath)) SdMan.remove(destPath);
  if (!SdMan.rename(tmpPath.c_str(), destPath)) {
    SdMan.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

bool SyncClient::ackJob(const char* jobId, const char* sha256Hex, const char* landscapeSha256Hex) {
  if (!piConfigured()) return false;

  WiFiClientSecure client;
  if (!configureClientForEndpoint(client, Endpoint::Pi)) return false;

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  String url = String(cfg_.piBaseUrl) + "/jobs/" + jobId + "/ack";
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", buildAuthHeader(cfg_.deviceToken));
  http.addHeader("X-Device-Id", cfg_.deviceId);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["sha256"] = sha256Hex;
  if (landscapeSha256Hex != nullptr) doc["landscape_sha256"] = landscapeSha256Hex;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();
  return code == 200;
}

ApprovalSubmitResult SyncClient::submitApproval(const store::ApprovalEntry& entry, Endpoint endpoint) {
  ApprovalSubmitResult result;
  bool useRelay = (endpoint == Endpoint::Relay);
  if (useRelay && !relayConfigured()) return result;
  if (!useRelay && !piConfigured()) return result;

  WiFiClientSecure client;
  if (!configureClientForEndpoint(client, endpoint)) return result;

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  String url;
  if (useRelay) {
    url = String(cfg_.relayBaseUrl) + "/accounts/" + cfg_.relayAccountId + "/approvals";
  } else {
    url = String(cfg_.piBaseUrl) + "/approvals";
  }
  if (!http.begin(client, url)) return result;

  http.addHeader("Content-Type", "application/json");
  if (useRelay) {
    http.addHeader("Authorization", buildAuthHeader(cfg_.relayAccountToken));
  } else {
    http.addHeader("Authorization", buildAuthHeader(cfg_.deviceToken));
    http.addHeader("X-Device-Id", cfg_.deviceId);
  }

  JsonDocument doc;
  doc["approval_id"] = entry.approvalId;
  doc["device_id"] = cfg_.deviceId;
  doc["job_id"] = entry.jobId;
  doc["action"] = store::approvalActionName(entry.action);
  doc["created_at"] = entry.createdAt;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code != 200) {
    http.end();
    return result;
  }

  result.networkOk = true;

  JsonDocument respDoc;
  if (!deserializeJson(respDoc, http.getStream())) {
    const char* status = respDoc["status"] | "";
    if (std::strcmp(status, "applied") == 0 || std::strcmp(status, "queued") == 0) {
      result.applied = true;
    } else if (std::strcmp(status, "already_applied") == 0) {
      result.applied = true;
      result.alreadyApplied = true;
    }
  }
  http.end();
  return result;
}

}  // namespace net
