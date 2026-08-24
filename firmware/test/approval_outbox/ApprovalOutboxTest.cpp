#include "testutil.h"
#include <cstdio>
#include <cstring>

#include "store/ApprovalOutbox.h"

using store::ApprovalAction;
using store::ApprovalEntry;
using store::ApprovalOutboxIndex;
using store::kMaxOutboxEntries;

namespace {

ApprovalEntry makeEntry(int n, const char* jobId, ApprovalAction action) {
  ApprovalEntry e;
  std::snprintf(e.approvalId, sizeof(e.approvalId), "appr%027d", n);
  std::snprintf(e.jobId, sizeof(e.jobId), "%s", jobId);
  e.action = action;
  e.createdAt = 1737590000 + n;
  e.synced = false;
  return e;
}

}  // namespace

int main() {
  ApprovalOutboxIndex outbox;
  CHECK(outbox.count() == 0);
  CHECK(!outbox.full());

  // Durable-before-network invariant: append() itself never talks to the
  // network — the approvalId is already fixed by the time this is called
  // (see header comment), which is what makes a reboot mid-sync safe.
  ApprovalEntry e1 = makeEntry(1, "job-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", ApprovalAction::Print);
  CHECK(outbox.append(e1));
  CHECK(outbox.count() == 1);
  CHECK(outbox.countUnsynced() == 1);

  // A second action on the same job before the first syncs is visible via
  // hasPendingForJob, so the UI can prevent contradictory queued actions.
  CHECK(outbox.hasPendingForJob("job-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  CHECK(!outbox.hasPendingForJob("job-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));

  // Fill to capacity.
  for (size_t i = 2; i <= kMaxOutboxEntries; i++) {
    char jobId[33];
    std::snprintf(jobId, sizeof(jobId), "job-%028zu", i);
    CHECK(outbox.append(makeEntry(static_cast<int>(i), jobId, ApprovalAction::Keep)));
  }
  CHECK(outbox.count() == kMaxOutboxEntries);
  CHECK(outbox.full());

  // Full outbox rejects a new approval rather than silently dropping one
  // (docs/architecture.md: "the UI surfaces inbox/outbox full").
  ApprovalEntry overflow = makeEntry(9999, "job-cccccccccccccccccccccccccccccc", ApprovalAction::Delete);
  CHECK(outbox.append(overflow) == false);

  // Marking synced doesn't free capacity by itself...
  CHECK(outbox.markSynced(e1.approvalId));
  CHECK(outbox.count() == kMaxOutboxEntries);
  CHECK(outbox.countUnsynced() == kMaxOutboxEntries - 1);

  // ...compactSynced() does, and only removes synced entries.
  size_t removed = outbox.compactSynced();
  CHECK(removed == 1);
  CHECK(outbox.count() == kMaxOutboxEntries - 1);
  CHECK(outbox.countUnsynced() == kMaxOutboxEntries - 1);  // unchanged: no unsynced entry was touched
  CHECK(outbox.append(overflow));  // room again

  // markSynced on an unknown id is a no-op, not a crash.
  CHECK(outbox.markSynced("unknown-approval-id") == false);

  // compactSynced() never drops an unsynced entry, even if every OTHER
  // entry is synced — a partially-successful sync pass must retry exactly
  // the entries that didn't confirm, next wake.
  ApprovalOutboxIndex partial;
  ApprovalEntry a = makeEntry(1, "job-1", ApprovalAction::Print);
  ApprovalEntry b = makeEntry(2, "job-2", ApprovalAction::Keep);
  partial.append(a);
  partial.append(b);
  partial.markSynced(a.approvalId);
  size_t partialRemoved = partial.compactSynced();
  CHECK(partialRemoved == 1);
  CHECK(partial.count() == 1);
  CHECK(!partial.at(0).synced);
  CHECK(std::strncmp(partial.at(0).approvalId, b.approvalId, sizeof(b.approvalId)) == 0);

  std::printf("ApprovalOutboxTest: all assertions passed\n");
  return 0;
}
