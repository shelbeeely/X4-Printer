#include "testutil.h"
#include <cstdio>
#include <cstring>

#include "store/JobStore.h"

using store::JobEntry;
using store::JobIndex;
using store::JobStatus;
using store::kMaxInboxJobs;

namespace {

JobEntry makeEntry(int n) {
  JobEntry e;
  std::snprintf(e.jobId, sizeof(e.jobId), "job%028d", n);
  std::snprintf(e.title, sizeof(e.title), "Document %d", n);
  std::snprintf(e.xtcPath, sizeof(e.xtcPath), "/inbox/%s.xtc", e.jobId);
  e.xtcBytes = 1000 + n;
  e.pageCount = 1;
  e.createdAt = 1737590000 + n;
  e.status = JobStatus::Downloaded;
  return e;
}

}  // namespace

int main() {
  JobIndex index;
  CHECK(index.count() == 0);
  CHECK(!index.full());
  CHECK(index.find("nonexistent") == nullptr);

  // Insert up to capacity.
  for (size_t i = 0; i < kMaxInboxJobs; i++) {
    JobEntry e = makeEntry(static_cast<int>(i));
    bool ok = index.upsert(e);
    CHECK(ok);
  }
  CHECK(index.count() == kMaxInboxJobs);
  CHECK(index.full());

  // One more distinct job must be rejected (capacity bound from
  // docs/architecture.md "Memory budget" — never silently grows).
  JobEntry overflow = makeEntry(9999);
  CHECK(index.upsert(overflow) == false);
  CHECK(index.count() == kMaxInboxJobs);

  // Re-syncing an EXISTING job (same jobId) must still succeed even when
  // full — this is a re-download/overwrite, not a growth.
  JobEntry existingUpdated = makeEntry(0);
  std::snprintf(existingUpdated.title, sizeof(existingUpdated.title), "Updated Title");
  CHECK(index.upsert(existingUpdated) == true);
  const JobEntry* found = index.find(existingUpdated.jobId);
  CHECK(found != nullptr);
  CHECK(std::strcmp(found->title, "Updated Title") == 0);
  CHECK(index.count() == kMaxInboxJobs);  // unchanged

  // Status transitions.
  CHECK(index.setStatus(existingUpdated.jobId, JobStatus::ApprovedPrint));
  CHECK(index.find(existingUpdated.jobId)->status == JobStatus::ApprovedPrint);
  CHECK(index.setStatus("does-not-exist", JobStatus::ApprovedKeep) == false);

  // Remove frees capacity.
  CHECK(index.remove(existingUpdated.jobId));
  CHECK(index.count() == kMaxInboxJobs - 1);
  CHECK(index.find(existingUpdated.jobId) == nullptr);
  CHECK(index.upsert(overflow) == true);  // now there's room
  CHECK(index.count() == kMaxInboxJobs);

  CHECK(index.remove("does-not-exist") == false);

  std::printf("JobStoreTest: all assertions passed\n");
  return 0;
}
