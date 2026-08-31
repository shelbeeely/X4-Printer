#include "sync/SyncManager.h"

#include <Arduino.h>

#include <cstring>
#include <ctime>

#include "calendar/CalendarSync.h"
#include "config/CalendarConfig.h"
#include "config/WifiStore.h"
#include "net/WifiManager.h"

namespace syncmgr {

namespace {
constexpr size_t kSyncBatchSize = 16;

// This firmware never anchors its clock any other way (no battery-backed
// RTC chip, and deep sleep only keeps a running *elapsed-time* counter,
// not an absolute one) -- calendar/CalendarSync.cpp needs a real wall-
// clock "now" to compute a meaningful RRULE window against, so this is
// called once per wake, while Wi-Fi is already connected for job sync,
// before that sync runs. Bounded: a network that can reach the Pi but not
// an NTP pool (unusual, but not impossible on a locked-down network)
// degrades to "skip calendar sync this wake" (see kMinPlausibleNow in
// CalendarSync.cpp) rather than blocking job sync indefinitely.
void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  const uint32_t deadline = millis() + 5000;
  while (time(nullptr) < 1700000000 && millis() < deadline) {
    delay(100);
  }
}
}  // namespace

SyncSummary SyncManager::runFullSync() {
  SyncSummary summary;

  if (!cfg_.loaded) {
    return summary;  // device not paired yet — nothing to sync
  }

  net::WifiManager wifi;
  if (!wifi.connect()) {
    return summary;  // no saved/visible network — normal offline state
  }
  summary.wifiConnected = true;

  net::SyncClient client(cfg_);

  downloadPendingJobs(client, summary);
  drainApprovalOutbox(client, summary);

  // docs/architecture.md step 7: one bounded extra pass if this wake
  // actually changed something the Pi might react to, so an approval that
  // unlocks a new job doesn't have to wait a whole extra wake cycle. Never
  // more than this single repeat.
  if (summary.approvalsSynced > 0) {
    downloadPendingJobs(client, summary);
  }

  // Pi-managed calendar/Wi-Fi config (docs/protocol.md §1.6) -- before the
  // calendar sync below, so a feed list edited on the Pi's admin console
  // takes effect the same wake it's pulled, not the one after.
  syncDeviceConfig(client);

  // Calendar sync (docs/architecture.md's idle-screen "next event" widget,
  // ui/InboxUI.cpp) rides this same connected window rather than opening
  // a second one -- runs after the print-inbox sync above, which is this
  // project's actual purpose, so a slow/unreachable calendar feed can
  // never delay it.
  syncClock();
  calendar::syncCalendars(config::CalendarConfig::instance());

  wifi.disconnect();
  return summary;
}

void SyncManager::downloadPendingJobs(net::SyncClient& client, SyncSummary& summary) {
  net::JobManifest manifests[kSyncBatchSize];
  int count = client.fetchPendingJobs(manifests, kSyncBatchSize);
  if (count <= 0) return;

  bool anyChange = false;
  for (int i = 0; i < count; i++) {
    const net::JobManifest& m = manifests[i];

    if (jobs_.full() && jobs_.find(m.jobId) == nullptr) {
      // Inbox full: leave this job pending on the server (never acked) so
      // it's retried once the user frees capacity (archives/deletes
      // something). See docs/architecture.md "Memory budget".
      continue;
    }

    String destPath = String("/inbox/") + m.jobId + ".xtc";
    bool ok = client.downloadJobToSd(m.jobId, destPath.c_str(), m.xtcSha256, m.xtcBytes);
    if (!ok) {
      summary.jobsFailedVerification++;
      continue;  // stays pending on the server, retried next wake
    }

    // Landscape-strip variant (docs/protocol.md §1.1/§4) -- optional, only
    // present in the manifest when the Pi actually produced one for this
    // job. All-or-nothing with the normal download above: a job only ever
    // becomes visible on-device once every variant the manifest advertised
    // is fully downloaded and verified, so a JobEntry never claims a
    // landscape variant that isn't really on SD.
    bool hasLandscape = m.landscapeXtcSha256[0] != '\0';
    String landscapeDestPath = String("/inbox/") + m.jobId + "_l.xtc";
    if (hasLandscape) {
      bool landscapeOk = client.downloadJobToSd(m.jobId, landscapeDestPath.c_str(), m.landscapeXtcSha256,
                                                 m.landscapeXtcBytes, "landscape");
      if (!landscapeOk) {
        summary.jobsFailedVerification++;
        continue;
      }
    }

    if (!client.ackJob(m.jobId, m.xtcSha256, hasLandscape ? m.landscapeXtcSha256 : nullptr)) {
      // Downloaded and verified locally but the ack didn't make it to the
      // Pi — the Pi will offer it again next wake (it's still "pending"
      // there), and our own downloadJobToSd() overwrite-on-verify is
      // idempotent, so this just costs a redundant download next time,
      // never a corrupt or duplicate local file.
      summary.jobsFailedVerification++;
      continue;
    }

    store::JobEntry entry;
    std::strncpy(entry.jobId, m.jobId, sizeof(entry.jobId) - 1);
    std::strncpy(entry.title, m.title, sizeof(entry.title) - 1);
    std::strncpy(entry.xtcPath, destPath.c_str(), sizeof(entry.xtcPath) - 1);
    entry.xtcBytes = m.xtcBytes;
    std::strncpy(entry.xtcSha256, m.xtcSha256, sizeof(entry.xtcSha256) - 1);
    entry.pageCount = m.pageCount;
    entry.createdAt = m.createdAt;
    entry.status = store::JobStatus::Downloaded;
    if (hasLandscape) {
      std::strncpy(entry.landscapeXtcPath, landscapeDestPath.c_str(), sizeof(entry.landscapeXtcPath) - 1);
      entry.landscapeXtcBytes = m.landscapeXtcBytes;
      std::strncpy(entry.landscapeXtcSha256, m.landscapeXtcSha256, sizeof(entry.landscapeXtcSha256) - 1);
      entry.landscapePageCount = m.landscapePageCount;
    }

    if (jobs_.upsert(entry)) {
      summary.newJobsDownloaded++;
      anyChange = true;
      store::saveJobIndex(jobs_);  // persist immediately: crash-safe per-job
    }
  }

  (void)anyChange;
}

void SyncManager::drainApprovalOutbox(net::SyncClient& client, SyncSummary& summary) {
  if (outbox_.count() == 0) return;

  bool piReachable = client.statusCheck(net::Endpoint::Pi);
  bool relayReachable = !piReachable && cfg_.hasRelay && client.statusCheck(net::Endpoint::Relay);
  if (!piReachable && !relayReachable) return;  // nothing reachable; retry next wake

  net::Endpoint endpoint = piReachable ? net::Endpoint::Pi : net::Endpoint::Relay;
  bool changed = false;

  for (size_t i = 0; i < outbox_.count(); i++) {
    const store::ApprovalEntry& entry = outbox_.at(i);
    if (entry.synced) continue;

    net::ApprovalSubmitResult result = client.submitApproval(entry, endpoint);
    if (result.applied) {
      outbox_.markSynced(entry.approvalId);
      summary.approvalsSynced++;
      summary.usedRelay = (endpoint == net::Endpoint::Relay);
      changed = true;
    } else {
      summary.approvalsFailedSync++;
    }
  }

  if (changed) {
    outbox_.compactSynced();
    store::saveApprovalOutbox(outbox_);
  }
}

void SyncManager::syncDeviceConfig(net::SyncClient& client) {
  net::DeviceConfigManifest manifest;
  if (!client.fetchDeviceConfig(manifest)) return;  // unreachable/unpaired -- leave existing config as-is

  // Calendars: wholesale replace -- see config/CalendarConfig.h's
  // replaceAll() comment for why this is safe (no on-device add path to
  // clobber).
  config::CalendarConfig::instance().replaceAll(manifest.calendars, manifest.calendarCount);
  config::CalendarConfig::instance().save();

  // Wi-Fi: merge only, via the exact same addOrUpdate() the Settings
  // screen's on-device flows use -- never a replace, so a Pi-side list
  // that's missing the network this device is currently on can't strand
  // it (see docs/protocol.md §1.6).
  if (manifest.wifiCount > 0) {
    config::WifiStore& wifiStore = config::WifiStore::instance();
    for (size_t i = 0; i < manifest.wifiCount; i++) {
      wifiStore.addOrUpdate(manifest.wifiNetworks[i].ssid, manifest.wifiNetworks[i].password);
    }
    wifiStore.save();
  }
}

}  // namespace syncmgr
