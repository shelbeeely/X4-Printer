#pragma once
// Durable offline approval queue — freestanding capacity/state logic, same
// split rationale as JobStore.h. See docs/protocol.md §3 "Idempotency
// summary": the approvalId is generated here, on-device, the moment the
// user makes a choice, BEFORE any network attempt — that is what makes a
// reboot mid-sync safe (the id survives in the persisted outbox and is
// retried unchanged, never regenerated).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "store/IdTypes.h"

namespace store {

constexpr size_t kMaxOutboxEntries = 32;

enum class ApprovalAction : uint8_t { Print = 0, Keep = 1, Delete = 2 };

inline const char* approvalActionName(ApprovalAction a) {
  switch (a) {
    case ApprovalAction::Print:
      return "print";
    case ApprovalAction::Keep:
      return "keep";
    case ApprovalAction::Delete:
      return "delete";
  }
  return "keep";
}

struct ApprovalEntry {
  char approvalId[kApprovalIdLen + 1] = {0};
  char jobId[kJobIdLen + 1] = {0};
  ApprovalAction action = ApprovalAction::Keep;
  uint32_t createdAt = 0;
  bool synced = false;  // true once the Pi/relay has confirmed applied/already_applied

  bool approvalIdEquals(const char* other) const { return std::strncmp(approvalId, other, kApprovalIdLen) == 0; }
};

// Fixed-capacity, allocation-free durable outbox. `full()` gates new
// approvals at the UI layer with an explicit "sync before approving more"
// message rather than silently dropping one — losing an approval the user
// believes they made is exactly the failure mode this store exists to
// prevent.
class ApprovalOutboxIndex {
 public:
  size_t count() const { return count_; }
  bool full() const { return count_ >= kMaxOutboxEntries; }

  bool append(const ApprovalEntry& entry) {
    if (full()) return false;
    entries_[count_++] = entry;
    return true;
  }

  // True if a not-yet-synced approval already exists for this job (any
  // action) — the UI uses this to grey out further actions on a document
  // that already has a pending approval, rather than letting the user
  // queue contradictory actions (Print then Delete) before either syncs.
  bool hasPendingForJob(const char* jobId) const {
    for (size_t i = 0; i < count_; i++) {
      if (!entries_[i].synced && std::strncmp(entries_[i].jobId, jobId, kJobIdLen) == 0) return true;
    }
    return false;
  }

  bool markSynced(const char* approvalId) {
    for (size_t i = 0; i < count_; i++) {
      if (entries_[i].approvalIdEquals(approvalId)) {
        entries_[i].synced = true;
        return true;
      }
    }
    return false;
  }

  size_t countUnsynced() const {
    size_t n = 0;
    for (size_t i = 0; i < count_; i++) {
      if (!entries_[i].synced) n++;
    }
    return n;
  }

  const ApprovalEntry& at(size_t i) const { return entries_[i]; }

  // Drops every synced entry, compacting the array. Called after a
  // successful sync pass to reclaim capacity — never drops an unsynced
  // entry, so a sync that only partially succeeds (network drop mid-drain)
  // still has every remaining entry retried next wake.
  size_t compactSynced() {
    size_t writeIdx = 0;
    size_t removed = 0;
    for (size_t readIdx = 0; readIdx < count_; readIdx++) {
      if (entries_[readIdx].synced) {
        removed++;
        continue;
      }
      if (writeIdx != readIdx) entries_[writeIdx] = entries_[readIdx];
      writeIdx++;
    }
    count_ = writeIdx;
    return removed;
  }

  void clear() { count_ = 0; }

 private:
  ApprovalEntry entries_[kMaxOutboxEntries];
  size_t count_ = 0;
};

// SD persistence (ApprovalOutbox.cpp). Path: /system/outbox.json. See
// JobStore.h for the same load/save split rationale.
bool loadApprovalOutbox(ApprovalOutboxIndex& outbox);
bool saveApprovalOutbox(const ApprovalOutboxIndex& outbox);

}  // namespace store
