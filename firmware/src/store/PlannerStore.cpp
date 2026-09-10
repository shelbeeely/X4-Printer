// SD-backed persistence for TaskIndex (PlannerStore.h holds the
// freestanding, host-tested capacity/lookup logic; this file is the thin
// Arduino/FreeInk glue that loads/saves it as JSON, mirroring
// JobStore.cpp exactly).

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

#include "store/AtomicJsonFile.h"
#include "store/PlannerStore.h"

namespace store {

constexpr const char* kPlannerIndexPath = "/planner/index.json";

// Loads /planner/index.json into `index`. Returns false if the file is
// missing (normal before the first Pi sync) or malformed (treated the
// same as missing -- start with an empty task list rather than fail to
// boot).
bool loadPlannerIndex(TaskIndex& index) {
  index.clear();
  if (!SdMan.exists(kPlannerIndexPath)) return false;
  String raw = SdMan.readFile(kPlannerIndexPath);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  JsonArrayConst tasks = doc["tasks"].as<JsonArrayConst>();
  for (JsonObjectConst t : tasks) {
    TaskEntry e;
    std::strncpy(e.id, t["id"] | "", sizeof(e.id) - 1);
    std::strncpy(e.title, t["title"] | "", sizeof(e.title) - 1);
    Category category = Category::Other;
    parseCategoryName(t["category"] | "Other", category);  // defaults to Other on missing/bad value
    e.category = category;
    std::strncpy(e.startTime, t["start_time"] | "", sizeof(e.startTime) - 1);
    std::strncpy(e.endTime, t["end_time"] | "", sizeof(e.endTime) - 1);
    e.done = t["done"] | false;
    if (e.id[0] == '\0') continue;
    index.upsert(e);  // never fails to grow past capacity here: the file was itself capacity-bounded when written
  }
  return true;
}

bool savePlannerIndex(const TaskIndex& index) {
  JsonDocument doc;
  JsonArray tasks = doc["tasks"].to<JsonArray>();
  for (size_t i = 0; i < index.count(); i++) {
    const TaskEntry& e = index.at(i);
    JsonObject t = tasks.add<JsonObject>();
    t["id"] = e.id;
    t["title"] = e.title;
    t["category"] = categoryName(e.category);
    t["start_time"] = e.startTime;
    t["end_time"] = e.endTime;
    t["done"] = e.done;
  }

  String out;
  serializeJson(doc, out);
  SdMan.ensureDirectoryExists("/planner");
  return store::writeFileAtomic(kPlannerIndexPath, out);
}

}  // namespace store
