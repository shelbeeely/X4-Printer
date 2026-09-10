#include "testutil.h"
#include <cstdio>
#include <cstring>

#include "store/PlannerStore.h"

using store::Category;
using store::kMaxPlannerTasks;
using store::TaskEntry;
using store::TaskIndex;

namespace {

TaskEntry makeEntry(int n) {
  TaskEntry e;
  std::snprintf(e.id, sizeof(e.id), "task%028d", n);
  std::snprintf(e.title, sizeof(e.title), "Task %d", n);
  e.category = static_cast<Category>(n % 8);
  std::snprintf(e.startTime, sizeof(e.startTime), "%02d:00", n % 24);
  std::snprintf(e.endTime, sizeof(e.endTime), "%02d:30", n % 24);
  e.done = (n % 2 == 0);
  return e;
}

}  // namespace

int main() {
  TaskIndex index;
  CHECK(index.count() == 0);
  CHECK(!index.full());
  CHECK(index.find("nonexistent") == nullptr);

  // Insert up to capacity.
  for (size_t i = 0; i < kMaxPlannerTasks; i++) {
    TaskEntry e = makeEntry(static_cast<int>(i));
    CHECK(index.upsert(e));
  }
  CHECK(index.count() == kMaxPlannerTasks);
  CHECK(index.full());

  // Category/time fields survive upsert/find unchanged.
  TaskEntry expected0 = makeEntry(0);
  const TaskEntry* found0 = index.find(expected0.id);
  CHECK(found0 != nullptr);
  CHECK(found0->category == expected0.category);
  CHECK(std::strcmp(found0->startTime, expected0.startTime) == 0);
  CHECK(std::strcmp(found0->endTime, expected0.endTime) == 0);
  CHECK(found0->done == expected0.done);

  // One more distinct task must be rejected once full.
  TaskEntry overflow = makeEntry(9999);
  CHECK(index.upsert(overflow) == false);
  CHECK(index.count() == kMaxPlannerTasks);

  // Re-syncing an EXISTING task (same id) still succeeds even when full.
  TaskEntry existingUpdated = makeEntry(0);
  std::snprintf(existingUpdated.title, sizeof(existingUpdated.title), "Updated Title");
  CHECK(index.upsert(existingUpdated) == true);
  const TaskEntry* found = index.find(existingUpdated.id);
  CHECK(found != nullptr);
  CHECK(std::strcmp(found->title, "Updated Title") == 0);
  CHECK(index.count() == kMaxPlannerTasks);  // unchanged

  // setDone.
  CHECK(index.setDone(existingUpdated.id, true));
  CHECK(index.find(existingUpdated.id)->done == true);
  CHECK(index.setDone(existingUpdated.id, false));
  CHECK(index.find(existingUpdated.id)->done == false);
  CHECK(index.setDone("does-not-exist", true) == false);

  // Remove frees capacity.
  CHECK(index.remove(existingUpdated.id));
  CHECK(index.count() == kMaxPlannerTasks - 1);
  CHECK(index.find(existingUpdated.id) == nullptr);
  CHECK(index.upsert(overflow) == true);  // now there's room
  CHECK(index.count() == kMaxPlannerTasks);

  CHECK(index.remove("does-not-exist") == false);

  // categoryName()/parseCategoryName() round-trip for all 8 values.
  const Category kAll[8] = {Category::Work,   Category::Break,  Category::Chore,   Category::Health,
                             Category::Social, Category::School, Category::Personal, Category::Other};
  for (Category c : kAll) {
    const char* name = store::categoryName(c);
    Category parsed;
    CHECK(store::parseCategoryName(name, parsed));
    CHECK(parsed == c);
  }
  Category ignored;
  CHECK(store::parseCategoryName("not-a-real-category", ignored) == false);

  std::printf("PlannerStoreTest: all assertions passed\n");
  return 0;
}
