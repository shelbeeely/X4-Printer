#include "store/TaskStore.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <cstring>

namespace store {

namespace {

constexpr const char* kPath = "/schedule.json";
constexpr const char* kTmpPath = "/schedule.json.tmp";

void taskToJson(const model::Task& t, JsonObject obj) {
  obj["label"] = t.label;
  obj["icon"] = t.iconId;
  obj["color"] = t.colorRgb565;
  obj["minutes"] = t.plannedMinutes;
  obj["done"] = t.done;

  JsonArray subs = obj["subtasks"].to<JsonArray>();
  for (uint8_t i = 0; i < t.subtaskCount; i++) {
    JsonObject s = subs.add<JsonObject>();
    s["label"] = t.subtasks[i].label;
    s["icon"] = t.subtasks[i].iconId;
    s["done"] = t.subtasks[i].done;
  }
}

void jsonToTask(JsonObjectConst obj, model::Task& t) {
  std::strncpy(t.label, obj["label"] | "", model::kMaxLabelLen - 1);
  t.label[model::kMaxLabelLen - 1] = '\0';
  t.iconId = obj["icon"] | 0;
  t.colorRgb565 = obj["color"] | 0;
  t.plannedMinutes = obj["minutes"] | 0;
  t.done = obj["done"] | false;

  t.subtaskCount = 0;
  for (JsonObjectConst s : obj["subtasks"].as<JsonArrayConst>()) {
    if (t.subtaskCount >= model::kMaxSubtasks) break;
    model::SubTask& sub = t.subtasks[t.subtaskCount++];
    std::strncpy(sub.label, s["label"] | "", model::kMaxLabelLen - 1);
    sub.label[model::kMaxLabelLen - 1] = '\0';
    sub.iconId = s["icon"] | 0;
    sub.done = s["done"] | false;
  }
}

}  // namespace

bool TaskStore::begin() {
  return LittleFS.begin(true);  // format-on-mount-failure covers first boot
}

bool TaskStore::save(const model::Schedule& schedule) {
  JsonDocument doc;
  JsonArray arr = doc["tasks"].to<JsonArray>();
  for (uint8_t i = 0; i < schedule.count(); i++) {
    taskToJson(schedule.at(i), arr.add<JsonObject>());
  }

  File f = LittleFS.open(kTmpPath, "w");
  if (!f) return false;
  bool wrote = serializeJson(doc, f) > 0;
  f.close();
  if (!wrote) {
    LittleFS.remove(kTmpPath);
    return false;
  }

  // Matches firmware/src/store/AtomicJsonFile.cpp's write-then-rename
  // pattern and its one documented gap: if remove() succeeds but the
  // rename below then fails, the next load() finds neither file and
  // starts empty (see load()'s header comment) rather than losing data
  // silently — a subsequent successful save() overwrites the orphaned
  // .tmp on its own next open("w").
  if (LittleFS.exists(kPath)) {
    if (!LittleFS.remove(kPath)) {
      LittleFS.remove(kTmpPath);
      return false;
    }
  }
  return LittleFS.rename(kTmpPath, kPath);
}

bool TaskStore::load(model::Schedule& out) {
  out.clear();

  File f = LittleFS.open(kPath, "r");
  if (!f) return false;  // normal on first boot — no schedule saved yet

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  for (JsonObjectConst obj : doc["tasks"].as<JsonArrayConst>()) {
    model::Task t;
    jsonToTask(obj, t);
    if (!out.add(t)) break;  // more saved tasks than kMaxTasks shouldn't happen, but never overrun
  }
  return true;
}

}  // namespace store
