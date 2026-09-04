#pragma once
// On-device planner task index -- the user-authored half of the Timeline
// screen's data (the other half is the existing calendar::CalendarSync
// "next event" cache; PlannerUI merges both, see ui/PlannerUI.h).
//
// Split the same way as JobStore.h: TaskIndex here is freestanding
// (fixed-capacity arrays, no heap allocation, no Arduino/FreeInk
// dependency) so it's host-unit-testable (firmware/test/planner_store/).
// The Arduino-dependent half -- PlannerStore.cpp -- owns loading/saving
// this state as JSON on the SD card via FreeInk's SDCardManager, following
// the same atomic write-then-rename pattern as JobStore.cpp.
//
// kMaxPlannerTasks bounds worst-case RAM the same way kMaxInboxJobs does
// for JobIndex -- see docs/architecture.md "Memory budget".

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "store/IdTypes.h"

namespace store {

constexpr size_t kMaxPlannerTasks = 64;
constexpr size_t kTaskTitleLen = 64;
constexpr size_t kTaskTimeLen = 5;  // "HH:MM"

// Fixed category set for icon+pattern coding (native screens) and color
// coding (the on-device web UI's planner.html). Order is part of the wire
// contract other units build against -- do not reorder or renumber.
enum class Category : uint8_t {
  Work = 0,
  Break,
  Chore,
  Health,
  Social,
  School,
  Personal,
  Other,
};

// Title-case, matching pi-server/focusink_server/planner.py's CATEGORIES
// tuple and docs/protocol.md §1.8's wire examples EXACTLY -- these strings
// round-trip over the sync API (GET .../planner/tasks), so firmware and
// Pi must agree byte-for-byte, not just on the same 8 names.
inline const char* categoryName(Category c) {
  switch (c) {
    case Category::Work:
      return "Work";
    case Category::Break:
      return "Break";
    case Category::Chore:
      return "Chore";
    case Category::Health:
      return "Health";
    case Category::Social:
      return "Social";
    case Category::School:
      return "School";
    case Category::Personal:
      return "Personal";
    case Category::Other:
      return "Other";
  }
  return "Other";
}

// Inverse of categoryName(). Returns false (out left unchanged) for
// anything unrecognized, so callers can default to Category::Other
// explicitly rather than have this function paper over a malformed value.
// Case-sensitive, matching categoryName()'s Title-case wire format exactly.
inline bool parseCategoryName(const char* s, Category& out) {
  if (std::strcmp(s, "Work") == 0) {
    out = Category::Work;
    return true;
  }
  if (std::strcmp(s, "Break") == 0) {
    out = Category::Break;
    return true;
  }
  if (std::strcmp(s, "Chore") == 0) {
    out = Category::Chore;
    return true;
  }
  if (std::strcmp(s, "Health") == 0) {
    out = Category::Health;
    return true;
  }
  if (std::strcmp(s, "Social") == 0) {
    out = Category::Social;
    return true;
  }
  if (std::strcmp(s, "School") == 0) {
    out = Category::School;
    return true;
  }
  if (std::strcmp(s, "Personal") == 0) {
    out = Category::Personal;
    return true;
  }
  if (std::strcmp(s, "Other") == 0) {
    out = Category::Other;
    return true;
  }
  return false;
}

struct TaskEntry {
  char id[kTaskIdLen + 1] = {0};
  char title[kTaskTitleLen + 1] = {0};
  Category category = Category::Other;
  // "HH:MM" wall-clock strings, entered as-is on the Pi and rendered as-is
  // on-device -- no timezone conversion happens anywhere, the same
  // no-timezone-support limitation ui/InboxUI.cpp's idleScreenMessage()
  // already documents for calendar event times (this firmware never
  // configures a timezone/NTP offset). ui/TimelineMerge.h sorts these
  // against the calendar module's own (UTC) next-event time as plain
  // strings, with the same caveat -- see that file's header comment.
  char startTime[kTaskTimeLen + 1] = {0};
  char endTime[kTaskTimeLen + 1] = {0};
  bool done = false;

  bool taskIdEquals(const char* other) const { return std::strncmp(id, other, kTaskIdLen) == 0; }
};

// Fixed-capacity, allocation-free task index. Every mutating method
// reports success/failure explicitly rather than asserting/aborting, same
// contract as JobIndex.
class TaskIndex {
 public:
  size_t count() const { return count_; }
  size_t capacity() const { return kMaxPlannerTasks; }
  bool full() const { return count_ >= kMaxPlannerTasks; }

  const TaskEntry* find(const char* id) const {
    for (size_t i = 0; i < count_; i++) {
      if (entries_[i].taskIdEquals(id)) return &entries_[i];
    }
    return nullptr;
  }
  TaskEntry* find(const char* id) {
    return const_cast<TaskEntry*>(static_cast<const TaskIndex*>(this)->find(id));
  }

  // Inserts a new entry, or overwrites an existing one with the same id
  // (re-sync from the Pi). Returns false only when the index is full AND
  // id is not already present.
  bool upsert(const TaskEntry& entry) {
    if (TaskEntry* existing = find(entry.id)) {
      *existing = entry;
      return true;
    }
    if (full()) return false;
    entries_[count_++] = entry;
    return true;
  }

  bool remove(const char* id) {
    for (size_t i = 0; i < count_; i++) {
      if (entries_[i].taskIdEquals(id)) {
        entries_[i] = entries_[count_ - 1];
        count_--;
        return true;
      }
    }
    return false;
  }

  bool setDone(const char* id, bool done) {
    if (TaskEntry* e = find(id)) {
      e->done = done;
      return true;
    }
    return false;
  }

  const TaskEntry& at(size_t i) const { return entries_[i]; }

  void clear() { count_ = 0; }

 private:
  TaskEntry entries_[kMaxPlannerTasks];
  size_t count_ = 0;
};

// SD persistence (PlannerStore.cpp -- Arduino/FreeInk dependent, not part
// of the host-testable surface above). Path: /planner/index.json.
bool loadPlannerIndex(TaskIndex& index);
bool savePlannerIndex(const TaskIndex& index);

}  // namespace store
