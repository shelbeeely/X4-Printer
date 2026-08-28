#include "sync/SyncManager.h"

#include <Arduino.h>

#include <cstring>

#include "net/WifiManager.h"

namespace sync {

namespace {
constexpr size_t kSyncBatchSize = 16;
}

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

}  // namespace sync
