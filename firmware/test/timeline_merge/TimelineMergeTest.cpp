#include "testutil.h"
#include <cstdio>
#include <cstring>

#include "ui/TimelineMerge.h"

using store::Category;
using store::TaskEntry;
using store::TaskIndex;

namespace {

TaskEntry makeTask(const char* id, const char* title, Category category, const char* start, const char* end) {
  TaskEntry t;
  std::snprintf(t.id, sizeof(t.id), "%s", id);
  std::snprintf(t.title, sizeof(t.title), "%s", title);
  t.category = category;
  std::snprintf(t.startTime, sizeof(t.startTime), "%s", start);
  std::snprintf(t.endTime, sizeof(t.endTime), "%s", end);
  return t;
}

}  // namespace

int main() {
  // Empty everything -> zero items.
  {
    TaskIndex tasks;
    ui::TimelineItem out[ui::kMaxTimelineItems];
    size_t count = ui::buildTimelineItems(tasks, nullptr, out, ui::kMaxTimelineItems);
    CHECK(count == 0);
  }

  // Tasks alone, out-of-order insertion, sorted by start time on output.
  {
    TaskIndex tasks;
    tasks.upsert(makeTask("1", "Lunch", Category::Break, "12:00", "13:00"));
    tasks.upsert(makeTask("2", "Standup", Category::Work, "09:00", "09:15"));
    tasks.upsert(makeTask("3", "Gym", Category::Health, "17:00", "18:00"));

    ui::TimelineItem out[ui::kMaxTimelineItems];
    size_t count = ui::buildTimelineItems(tasks, nullptr, out, ui::kMaxTimelineItems);
    CHECK(count == 3);
    CHECK(std::strncmp(out[0].label, "09:00", 5) == 0);
    CHECK(std::strncmp(out[1].label, "12:00", 5) == 0);
    CHECK(std::strncmp(out[2].label, "17:00", 5) == 0);
    CHECK(out[0].category == Category::Work);
    CHECK(!out[0].isCalendarEvent);
  }

  // Calendar event merges into the correct sorted position among tasks.
  {
    TaskIndex tasks;
    tasks.upsert(makeTask("1", "Standup", Category::Work, "09:00", "09:15"));
    tasks.upsert(makeTask("2", "Gym", Category::Health, "17:00", "18:00"));

    config::NextEventInfo event;
    event.hasEvent = true;
    std::snprintf(event.title, sizeof(event.title), "Team sync");
    event.start = 12 * 3600;  // 12:00 UTC (epoch 1970-01-01)

    ui::TimelineItem out[ui::kMaxTimelineItems];
    size_t count = ui::buildTimelineItems(tasks, &event, out, ui::kMaxTimelineItems);
    CHECK(count == 3);
    CHECK(std::strncmp(out[0].label, "09:00", 5) == 0);
    CHECK(std::strncmp(out[1].label, "12:00", 5) == 0);
    CHECK(out[1].isCalendarEvent);
    CHECK(std::strstr(out[1].label, "Team sync") != nullptr);
    CHECK(std::strncmp(out[2].label, "17:00", 5) == 0);
  }

  // No event (hasEvent false) is skipped, not shown as a phantom row.
  {
    TaskIndex tasks;
    tasks.upsert(makeTask("1", "Standup", Category::Work, "09:00", "09:15"));
    config::NextEventInfo event;  // hasEvent defaults to false
    ui::TimelineItem out[ui::kMaxTimelineItems];
    size_t count = ui::buildTimelineItems(tasks, &event, out, ui::kMaxTimelineItems);
    CHECK(count == 1);
  }

  // done flag survives into the merged item.
  {
    TaskIndex tasks;
    TaskEntry t = makeTask("1", "Standup", Category::Work, "09:00", "09:15");
    t.done = true;
    tasks.upsert(t);
    ui::TimelineItem out[ui::kMaxTimelineItems];
    size_t count = ui::buildTimelineItems(tasks, nullptr, out, ui::kMaxTimelineItems);
    CHECK(count == 1);
    CHECK(out[0].done == true);
  }

  // Capacity bound: maxItems caps output even if more tasks exist.
  {
    TaskIndex tasks;
    tasks.upsert(makeTask("1", "A", Category::Work, "09:00", "09:15"));
    tasks.upsert(makeTask("2", "B", Category::Work, "10:00", "10:15"));
    ui::TimelineItem out[1];
    size_t count = ui::buildTimelineItems(tasks, nullptr, out, 1);
    CHECK(count == 1);
    CHECK(std::strncmp(out[0].label, "09:00", 5) == 0);
  }

  std::printf("TimelineMergeTest: all assertions passed\n");
  return 0;
}
