#pragma once
// HTTPS client for docs/protocol.md §1 (direct, Pi) and §2 (relay). This is
// the one place in firmware that talks to the network for sync purposes;
// everything else (SyncManager) is protocol-agnostic orchestration.
//
// Streaming discipline (docs/architecture.md "Memory budget"): downloadJobToSd
// never buffers a whole file. It reads the HTTP response body in
// kStreamChunkBytes chunks, writes each chunk straight to the open SD file,
// and folds it into a running mbedtls SHA-256 context — RAM cost is the
// chunk buffer (2KB) plus ~200B of hash state, regardless of file size.
// Modeled on crosspoint-reader's HttpDownloader::downloadToFile (streams to
// a HalFile the same way) — see docs/architecture.md's CrossPoint
// paragraph — but reimplemented from scratch against FreeInk's SDCardManager
// directly rather than depending on CrossPoint's own lib/.

#include <cstddef>
#include <cstdint>

#include <WString.h>

#include "config/CalendarConfig.h"
#include "config/DeviceConfig.h"
#include "config/WifiStore.h"
#include "store/ApprovalOutbox.h"

namespace net {

constexpr size_t kStreamChunkBytes = 2048;
constexpr uint32_t kHttpTimeoutMs = 15000;

// Fixed-capacity holding area for docs/protocol.md §1.6's response, sized
// to firmware's own on-device caps (config::kMaxCalendars/
// kMaxWifiNetworks) -- reuses config::CalendarFeed/WifiCredential directly
// rather than duplicating their size constants in a parallel struct here.
struct DeviceConfigManifest {
  config::CalendarFeed calendars[config::kMaxCalendars];
  size_t calendarCount = 0;
  config::WifiCredential wifiNetworks[config::kMaxWifiNetworks];
  size_t wifiCount = 0;
};

struct JobManifest {
  char jobId[33] = {0};
  char title[65] = {0};
  uint32_t createdAt = 0;
  uint32_t xtcBytes = 0;
  char xtcSha256[65] = {0};
  uint16_t pageCount = 0;

  // Optional landscape-strip variant (docs/protocol.md §1.1) -- empty
  // landscapeXtcSha256 means this job has none.
  uint32_t landscapeXtcBytes = 0;
  char landscapeXtcSha256[65] = {0};
  uint16_t landscapePageCount = 0;
};

enum class Endpoint { Pi, Relay };

struct ApprovalSubmitResult {
  bool networkOk = false;  // request completed (regardless of applied/rejected)
  // True once the endpoint durably owns this approval — the device may
  // mark the outbox entry synced() and stop retrying it. For the direct Pi
  // path this means the approval was actually applied ("applied" /
  // "already_applied"): CUPS was invoked (or the dedup path confirmed it
  // already had been). For the relay path this means the relay accepted
  // and durably stored the envelope ("queued") — the relay guarantees
  // eventual delivery to the Pi from here, so from the device's point of
  // view its job here is done; docs/protocol.md §2.4 covers how the device
  // can later confirm the Pi actually applied it, if it wants to.
  bool applied = false;
  bool alreadyApplied = false;
};

class SyncClient {
 public:
  explicit SyncClient(const config::DeviceConfigData& cfg) : cfg_(cfg) {}

  // Cheap reachability probe (docs/protocol.md §1.5) — used by SyncManager
  // to decide direct-vs-relay before attempting the heavier job listing.
  bool statusCheck(Endpoint endpoint);

  // Fills out[] with up to maxCount pending manifests. Returns the count
  // written, or -1 on network/parse failure.
  int fetchPendingJobs(JobManifest* out, size_t maxCount);

  // docs/protocol.md §1.6: the Pi-managed calendar/Wi-Fi lists. Returns
  // false on network/parse failure and leaves `out` untouched -- callers
  // must not clear on-device config just because this one sync couldn't
  // reach the Pi.
  bool fetchDeviceConfig(DeviceConfigManifest& out);

  // `variant`, when non-null, is sent as the sync API's `?variant=`
  // query param (docs/protocol.md §1.2) -- e.g. "landscape" to download
  // the landscape-strip rendering instead of the normal one.
  bool downloadJobToSd(const char* jobId, const char* destPath, const char* expectedSha256Hex, uint32_t expectedBytes,
                       const char* variant = nullptr);

  // `landscapeSha256Hex`, when non-null, is included in the ack body so
  // one ack call can cover both variants (docs/protocol.md §1.3).
  bool ackJob(const char* jobId, const char* sha256Hex, const char* landscapeSha256Hex = nullptr);

  ApprovalSubmitResult submitApproval(const store::ApprovalEntry& entry, Endpoint endpoint);

  // docs/protocol.md §1.7: hands a locally-created job's original image
  // bytes (SD file at `path`) to the Pi under this device's own `jobId`,
  // so the Pi's normal conversion pipeline can ingest it under the same
  // id this device already has an approval queued against (see
  // sync/SyncManager.cpp's uploadPendingOriginals()). Streams straight
  // from the open SD file (same "never buffer a whole file" discipline
  // downloadJobToSd's reverse direction follows) — Pi-direct only, never
  // attempted over the relay (the relay never sees document bytes).
  bool uploadOriginal(const char* jobId, const char* path, const char* mime, const char* title, uint32_t bytes);

 private:
  const config::DeviceConfigData& cfg_;

  bool piConfigured() const { return cfg_.loaded && cfg_.piBaseUrl[0] != '\0'; }
  bool relayConfigured() const { return cfg_.loaded && cfg_.hasRelay; }
};

}  // namespace net
