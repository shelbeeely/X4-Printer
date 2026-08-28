#pragma once
// On-device print-inbox job index.
//
// Split the same way as xtc/XtcFormat.h: JobIndex here is freestanding
// (fixed-capacity arrays, no heap allocation, no Arduino/FreeInk
// dependency) so the capacity/idempotency invariants that matter for
// correctness are host-unit-testable (firmware/test/job_store/). The
// Arduino-dependent half — JobStore in JobStore.cpp — owns loading/saving
// this state as JSON on the SD card via FreeInk's SDCardManager, following
// CrossPoint's PersistableStore pattern (atomic write-to-temp-then-rename;
// see docs/architecture.md's "CrossPoint Reader" paragraph).
//
// MAX_INBOX_JOBS bounds worst-case RAM for the whole index to a small,
// fixed footprint regardless of how many jobs the Pi has queued — see
// docs/architecture.md "Memory budget".

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "store/IdTypes.h"

namespace store {

constexpr size_t kMaxInboxJobs = 64;
constexpr size_t kTitleLen = 64;   // display title, truncated
constexpr size_t kPathLen = 48;    // "/inbox/<32 hex>.xtc" fits comfortably; the
                                    // landscape variant's "/inbox/<32 hex>_l.xtc"
                                    // (45 chars) still fits within this same length
constexpr size_t kSha256Len = 64;  // hex-encoded sha256

enum class JobStatus : uint8_t {
  Downloaded = 0,      // on SD, not yet reviewed
  ApprovedPrint = 1,   // user chose Print; approval enqueued in ApprovalOutbox
  ApprovedKeep = 2,    // user chose Keep
  ApprovedDelete = 3,  // user chose Delete/archive
};

struct JobEntry {
  char jobId[kJobIdLen + 1] = {0};
  char title[kTitleLen + 1] = {0};
  char xtcPath[kPathLen + 1] = {0};
  uint32_t xtcBytes = 0;
  char xtcSha256[kSha256Len + 1] = {0};
  uint16_t pageCount = 0;
  uint32_t createdAt = 0;
  JobStatus status = JobStatus::Downloaded;

  // Landscape-strip rendering (docs/protocol.md §4) -- a second, optional
  // XTC file meant to be read with the device turned 90 degrees. Empty
  // path means this job has none (converted before this feature existed,
  // or the Pi's landscape conversion failed for this document's page
  // shape) -- same "empty means not available" convention xtcPath's
  // sibling fields already use elsewhere in this codebase (e.g.
  // thumbnail_path on the Pi side).
  char landscapeXtcPath[kPathLen + 1] = {0};
  uint32_t landscapeXtcBytes = 0;
  char landscapeXtcSha256[kSha256Len + 1] = {0};
  uint16_t landscapePageCount = 0;

  bool jobIdEquals(const char* other) const { return std::strncmp(jobId, other, kJobIdLen) == 0; }
};

// Fixed-capacity, allocation-free job index. Every mutating method reports
// success/failure explicitly rather than asserting/aborting, so callers
// (the sync path, the UI) can surface "inbox full" to the user instead of
// crashing or silently dropping data.
class JobIndex {
 public:
  size_t count() const { return count_; }
  size_t capacity() const { return kMaxInboxJobs; }
  bool full() const { return count_ >= kMaxInboxJobs; }

  const JobEntry* find(const char* jobId) const {
    for (size_t i = 0; i < count_; i++) {
      if (entries_[i].jobIdEquals(jobId)) return &entries_[i];
    }
    return nullptr;
  }
  JobEntry* find(const char* jobId) {
    return const_cast<JobEntry*>(static_cast<const JobIndex*>(this)->find(jobId));
  }

  // Inserts a new entry, or overwrites an existing one with the same
  // jobId (re-sync after a partial/corrupt previous download). Returns
  // false only when the index is full AND jobId is not already present —
  // the caller should surface this as "inbox full" rather than silently
  // losing the job.
  bool upsert(const JobEntry& entry) {
    if (JobEntry* existing = find(entry.jobId)) {
      *existing = entry;
      return true;
    }
    if (full()) return false;
    entries_[count_++] = entry;
    return true;
  }

  bool remove(const char* jobId) {
    for (size_t i = 0; i < count_; i++) {
      if (entries_[i].jobIdEquals(jobId)) {
        entries_[i] = entries_[count_ - 1];
        count_--;
        return true;
      }
    }
    return false;
  }

  bool setStatus(const char* jobId, JobStatus status) {
    if (JobEntry* e = find(jobId)) {
      e->status = status;
      return true;
    }
    return false;
  }

  const JobEntry& at(size_t i) const { return entries_[i]; }

  void clear() { count_ = 0; }

 private:
  JobEntry entries_[kMaxInboxJobs];
  size_t count_ = 0;
};

// SD persistence (JobStore.cpp — Arduino/FreeInk dependent, not part of the
// host-testable surface above). Path: /inbox/index.json.
bool loadJobIndex(JobIndex& index);
bool saveJobIndex(const JobIndex& index);

}  // namespace store
