#include "testutil.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "store/ApprovalOutbox.h"

using store::ApprovalAction;
using store::ApprovalEntry;
using store::ApprovalOutboxIndex;
using store::EnqueueResult;
using store::JobEntry;
using store::JobIndex;
using store::JobStatus;
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

JobEntry makeJob(const char* jobId, const char* title) {
  JobEntry e;
  std::snprintf(e.jobId, sizeof(e.jobId), "%s", jobId);
  std::snprintf(e.title, sizeof(e.title), "%s", title);
  e.status = JobStatus::Downloaded;
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

  // enqueueApproval() — the shared function ui/InboxUI.cpp (physical
  // buttons) and ui/WebUiServer.cpp (on-device web UI) both call, so it's
  // the one place this durable-before-network sequence needs to be right.
  {
    JobIndex jobs;
    jobs.upsert(makeJob("job-web-aaaaaaaaaaaaaaaaaaaaaaaa", "Invoice"));
    ApprovalOutboxIndex ob;

    // Unknown job: nothing mutated.
    EnqueueResult r = store::enqueueApproval(jobs, ob, "job-does-not-exist-aaaaaaaaaaaaa", ApprovalAction::Print,
                                              "approval-1-aaaaaaaaaaaaaaaaaaaaa", 1000);
    CHECK(r == EnqueueResult::UnknownJob);
    CHECK(ob.count() == 0);

    // Ok: job transitions status, outbox gets exactly one durable entry
    // carrying the caller-supplied id/timestamp unchanged.
    r = store::enqueueApproval(jobs, ob, "job-web-aaaaaaaaaaaaaaaaaaaaaaaa", ApprovalAction::Print,
                                "approval-2-aaaaaaaaaaaaaaaaaaaaa", 2000);
    CHECK(r == EnqueueResult::Ok);
    CHECK(ob.count() == 1);
    CHECK(jobs.find("job-web-aaaaaaaaaaaaaaaaaaaaaaaa")->status == JobStatus::ApprovedPrint);
    CHECK(std::strcmp(ob.at(0).approvalId, "approval-2-aaaaaaaaaaaaaaaaaaaaa") == 0);
    CHECK(ob.at(0).createdAt == 2000);
    CHECK(ob.at(0).action == ApprovalAction::Print);

    // Already pending: a second action on the same job before the first
    // syncs is rejected, not silently queued as a contradictory action.
    r = store::enqueueApproval(jobs, ob, "job-web-aaaaaaaaaaaaaaaaaaaaaaaa", ApprovalAction::Delete,
                                "approval-3-aaaaaaaaaaaaaaaaaaaaa", 3000);
    CHECK(r == EnqueueResult::AlreadyPending);
    CHECK(ob.count() == 1);  // unchanged

    // Outbox full: a different, unblocked job is still rejected once the
    // outbox has no capacity left.
    ApprovalOutboxIndex fullOb;
    JobIndex manyJobs;
    for (size_t i = 0; i < kMaxOutboxEntries; i++) {
      char jobId[33];
      std::snprintf(jobId, sizeof(jobId), "job-full-%023zu", i);
      manyJobs.upsert(makeJob(jobId, "Doc"));
      fullOb.append(makeEntry(static_cast<int>(i), jobId, ApprovalAction::Keep));
    }
    CHECK(fullOb.full());
    manyJobs.upsert(makeJob("job-overflow-aaaaaaaaaaaaaaaaaaa", "Overflow"));
    r = store::enqueueApproval(manyJobs, fullOb, "job-overflow-aaaaaaaaaaaaaaaaaaa", ApprovalAction::Keep,
                                "approval-overflow-aaaaaaaaaaaaaa", 4000);
    CHECK(r == EnqueueResult::OutboxFull);
    CHECK(fullOb.count() == kMaxOutboxEntries);  // unchanged
    CHECK(manyJobs.find("job-overflow-aaaaaaaaaaaaaaaaaaa")->status == JobStatus::Downloaded);  // unchanged
  }

  // parseApprovalAction() — the inverse of approvalActionName(), used by
  // ui/WebUiServer.cpp to parse the web UI's POST /api/jobs body.
  {
    ApprovalAction a;
    CHECK(store::parseApprovalAction("print", a) && a == ApprovalAction::Print);
    CHECK(store::parseApprovalAction("keep", a) && a == ApprovalAction::Keep);
    CHECK(store::parseApprovalAction("delete", a) && a == ApprovalAction::Delete);
    CHECK(!store::parseApprovalAction("reprint", a));
    CHECK(!store::parseApprovalAction("", a));
    // Round-trips with approvalActionName() for every action value.
    for (ApprovalAction action : {ApprovalAction::Print, ApprovalAction::Keep, ApprovalAction::Delete}) {
      ApprovalAction roundTripped;
      CHECK(store::parseApprovalAction(store::approvalActionName(action), roundTripped));
      CHECK(roundTripped == action);
    }
  }

  std::printf("ApprovalOutboxTest: all assertions passed\n");
  return 0;
}
