#include "testutil.h"
#include <cstdio>
#include <cstring>

#include "model/Schedule.h"

using model::kMaxTasks;
using model::Schedule;
using model::Task;

namespace {

Task makeTask(const char* label, uint16_t minutes) {
  Task t;
  std::strncpy(t.label, label, sizeof(t.label) - 1);
  t.plannedMinutes = minutes;
  return t;
}

}  // namespace

int main() {
  Schedule s;
  CHECK(s.count() == 0);
  CHECK(s.totalPlannedMinutes() == 0);
  CHECK(s.findTaskAtMinute(0) == -1);  // empty schedule: nothing owns any minute

  CHECK(s.add(makeTask("Breakfast", 30)));
  CHECK(s.add(makeTask("School", 180)));
  CHECK(s.add(makeTask("Lunch", 30)));
  CHECK(s.count() == 3);
  CHECK(s.totalPlannedMinutes() == 240);

  // Fill to capacity, then reject the next add — never silently grows
  // past kMaxTasks (same fixed-capacity discipline as the main
  // X4-Printer firmware's store:: types).
  for (uint8_t i = s.count(); i < kMaxTasks; i++) {
    CHECK(s.add(makeTask("Filler", 1)));
  }
  CHECK(s.count() == kMaxTasks);
  CHECK(s.add(makeTask("Overflow", 1)) == false);
  CHECK(s.count() == kMaxTasks);

  // findTaskAtMinute: tasks laid out back-to-back in order.
  CHECK(s.findTaskAtMinute(0) == 0);        // start of Breakfast
  CHECK(s.findTaskAtMinute(29) == 0);       // still Breakfast
  CHECK(s.findTaskAtMinute(30) == 1);       // start of School
  CHECK(s.findTaskAtMinute(209) == 1);      // still School (30 + 180 - 1)
  CHECK(s.findTaskAtMinute(210) == 2);      // start of Lunch

  // markDone / removeAt / clear.
  s.markDone(0, true);
  CHECK(s.at(0).done);
  CHECK(s.removeAt(0));
  CHECK(s.count() == kMaxTasks - 1);
  CHECK(std::strcmp(s.at(0).label, "School") == 0);  // shifted down
  CHECK(s.removeAt(255) == false);                   // out of range, no-op

  s.clear();
  CHECK(s.count() == 0);
  CHECK(s.totalPlannedMinutes() == 0);

  std::puts("ScheduleTest OK");
  return 0;
}
