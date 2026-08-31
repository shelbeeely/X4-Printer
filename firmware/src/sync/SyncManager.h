#pragma once
// Orchestrates the full wake sequence from docs/architecture.md "Deep
// sleep / wake sequence": connect Wi-Fi, download+verify+ack pending jobs,
// drain the offline approval outbox (direct to the Pi, falling back to the
// relay if the Pi isn't reachable on the current network), disconnect.
// Bounded to at most two passes total (not unbounded), so a misbehaving
// server can't keep the radio on forever.

#include <cstdint>

#include "config/DeviceConfig.h"
#include "net/SyncClient.h"
#include "store/ApprovalOutbox.h"
#include "store/JobStore.h"

// Named syncmgr, not sync: ESP-IDF's newlib unistd.h declares a global
// ::sync() (POSIX sync(2)), which a top-level `namespace sync` collides
// with once Arduino.h pulls that header in transitively.
namespace syncmgr {

struct SyncSummary {
  bool wifiConnected = false;
  int newJobsDownloaded = 0;
  int jobsFailedVerification = 0;
  int approvalsSynced = 0;
  int approvalsFailedSync = 0;
  bool usedRelay = false;
};

class SyncManager {
 public:
  SyncManager(const config::DeviceConfigData& cfg, store::JobIndex& jobs, store::ApprovalOutboxIndex& outbox)
      : cfg_(cfg), jobs_(jobs), outbox_(outbox) {}

  // Persists jobs_/outbox_ to SD as it goes (after each successful job
  // download and once after draining approvals), so a mid-sequence power
  // loss loses at most the single in-flight operation, never previously
  // confirmed state. Safe to call with no saved Wi-Fi networks or an
  // unpaired device (returns immediately with wifiConnected=false).
  SyncSummary runFullSync();

 private:
  const config::DeviceConfigData& cfg_;
  store::JobIndex& jobs_;
  store::ApprovalOutboxIndex& outbox_;

  void downloadPendingJobs(net::SyncClient& client, SyncSummary& summary);
  void drainApprovalOutbox(net::SyncClient& client, SyncSummary& summary);
};

}  // namespace syncmgr
