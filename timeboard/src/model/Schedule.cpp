#include "model/Schedule.h"

#include <cstring>

namespace model {

bool Schedule::add(const Task& task) {
  if (count_ >= kMaxTasks) return false;
  tasks_[count_++] = task;
  return true;
}

bool Schedule::removeAt(uint8_t index) {
  if (index >= count_) return false;
  for (uint8_t i = index; i + 1 < count_; i++) {
    tasks_[i] = tasks_[i + 1];
  }
  count_--;
  tasks_[count_] = Task();  // don't leave a stale copy past the new end
  return true;
}

void Schedule::markDone(uint8_t index, bool done) {
  if (index >= count_) return;
  tasks_[index].done = done;
}

void Schedule::clear() {
  for (uint8_t i = 0; i < count_; i++) tasks_[i] = Task();
  count_ = 0;
}

uint32_t Schedule::totalPlannedMinutes() const {
  uint32_t total = 0;
  for (uint8_t i = 0; i < count_; i++) total += tasks_[i].plannedMinutes;
  return total;
}

int Schedule::findTaskAtMinute(uint32_t elapsedMinutes) const {
  uint32_t offset = 0;
  for (uint8_t i = 0; i < count_; i++) {
    offset += tasks_[i].plannedMinutes;
    if (elapsedMinutes < offset) return i;
  }
  return -1;
}

}  // namespace model
