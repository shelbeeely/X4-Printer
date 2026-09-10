// Wokwi-only smoke test firmware -- NOT part of the real device build.
//
// The host-side tests in firmware/test/ (job_store/, approval_outbox/,
// xtc_format/) only cover the freestanding, allocation-free logic split
// out specifically because it has no Arduino/SD/network dependency (see
// each header's own comment on that split). Everything on the other side
// of that split -- WifiManager's real WiFi.begin()/scanNetworks() calls,
// SyncClient's real HTTPS requests, SyncManager's real SD-backed
// downloads via SDCardManager -- has never run anywhere except a real X4,
// because none of it links on a plain Linux host.
//
// This build exercises that other half for real, without a real X4: it
// links the actual production config/, net/, store/, and sync/ sources
// (unmodified -- see platformio.ini's build_src_filter for
// [env:wokwi_sync_test]) into a stripped firmware image with no display,
// no buttons, no FreeInkUI -- just enough to call the same
// syncmgr::SyncManager::runFullSync() main.cpp's runSyncPass() calls, and
// print a single machine-readable PASS/FAIL line to serial that
// .github/workflows/tests.yml's wokwi-sync-smoke-test job (via
// wokwi/wokwi-ci-action) greps for. See docs/testing.md "Wokwi firmware
// simulation" for the full explanation, what this can and can't catch,
// and its known limitations.
//
// Provisioning (/system/device.json, /system/pi_ca.pem, /system/wifi.json)
// is baked onto the Wokwi virtual SD card image by the CI job before this
// firmware ever runs -- exactly the files pi-server/tools/pair_device.py
// and a real X4's Wi-Fi setup screen would produce, so DeviceConfig/
// WifiStore parse the identical on-disk format a real device would.

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdio>

#include "config/DeviceConfig.h"
#include "config/WifiStore.h"
#include "store/ApprovalOutbox.h"
#include "store/JobStore.h"
#include "sync/SyncManager.h"

// Same rationale as main.cpp's SET_LOOP_TASK_STACK_SIZE: runFullSync()
// nests HTTPClient/WiFiClientSecure/mbedTLS + ArduinoJson under this same
// call stack, well past the 16KB weak default.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

namespace {

store::JobIndex jobIndex;
store::ApprovalOutboxIndex outboxIndex;
config::DeviceConfigData deviceConfig;

void report(bool pass, const char* reason) {
  if (pass) {
    Serial.printf("WOKWI_SYNC_TEST: PASS (%s)\n", reason);
  } else {
    Serial.printf("WOKWI_SYNC_TEST: FAIL (%s)\n", reason);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Give the wokwi-ci-action's serial capture a moment to attach before
  // anything meaningful is printed.
  delay(500);

  if (!SdMan.begin()) {
    report(false, "SD card did not mount");
    return;
  }

  config::DeviceConfig::instance().load();
  deviceConfig = config::DeviceConfig::instance().data();
  if (!deviceConfig.loaded) {
    report(false, "device not paired: /system/device.json missing or invalid");
    return;
  }

  config::WifiStore::instance().load();
  if (config::WifiStore::instance().count() == 0) {
    report(false, "no saved Wi-Fi credentials: /system/wifi.json missing or empty");
    return;
  }

  store::loadJobIndex(jobIndex);
  store::loadApprovalOutbox(outboxIndex);

  // The real thing under test: production SyncManager/SyncClient/
  // WifiManager code, unmodified, run against whatever pi-server the CI
  // job pointed /system/device.json's pi_base_url at.
  syncmgr::SyncManager manager(deviceConfig, jobIndex, outboxIndex);
  syncmgr::SyncSummary summary = manager.runFullSync();

  if (!summary.wifiConnected) {
    report(false, "WiFiManager::connect() never reached WL_CONNECTED");
    return;
  }
  if (summary.jobsFailedVerification > 0) {
    report(false, "one or more jobs failed SHA-256 verification during download");
    return;
  }

  char reason[96];
  std::snprintf(reason, sizeof(reason), "jobs_downloaded=%d approvals_synced=%d", summary.newJobsDownloaded,
                summary.approvalsSynced);
  report(true, reason);
}

void loop() {
  // Nothing left to do -- the CI job's expect_text/fail_text watches the
  // single line setup() already printed. Idle rather than looping the sync
  // pass again, so the pass/fail signal stays unambiguous.
  delay(1000);
}
