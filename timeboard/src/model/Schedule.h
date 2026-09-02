#pragma once
// A fixed-capacity, ordered list of Tasks representing one day's plan —
// what ui/TimelineView.h renders as a color-coded strip. Deliberately not
// wall-clock-aware itself: callers track "minutes since the schedule
// started" (main.cpp) and pass that in, so this class stays pure logic
// and host-testable (test/schedule/ScheduleTest.cpp) without pulling in
// an RTC/NTP dependency.

#include <cstdint>

#include "model/Task.h"

namespace model {

constexpr uint8_t kMaxTasks = 16;

class Schedule {
 public:
  // Appends a task. Returns false (no-op) if already at kMaxTasks — the
  // caller (main.cpp's UI flow) is expected to surface "schedule full."
  bool add(const Task& task);

  bool removeAt(uint8_t index);
  void markDone(uint8_t index, bool done);
  void clear();

  uint8_t count() const { return count_; }
  Task& at(uint8_t index) { return tasks_[index]; }
  const Task& at(uint8_t index) const { return tasks_[index]; }

  uint32_t totalPlannedMinutes() const;

  // Index of the task that owns a given elapsed-minutes offset into the
  // schedule (tasks laid out back-to-back in order, no gaps), or -1 if
  // elapsedMinutes is at or past the end. Used to know which task is
  // "current" for focus-lock purposes and where to draw the timeline's
  // "now" marker.
  int findTaskAtMinute(uint32_t elapsedMinutes) const;

 private:
  Task tasks_[kMaxTasks];
  uint8_t count_ = 0;
};

}  // namespace model
