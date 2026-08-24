#include "store/ApprovalOutbox.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

#include "store/AtomicJsonFile.h"

namespace store {

constexpr const char* kOutboxPath = "/system/outbox.json";

bool loadApprovalOutbox(ApprovalOutboxIndex& outbox) {
  outbox.clear();
  if (!SdMan.exists(kOutboxPath)) return false;
  String raw = SdMan.readFile(kOutboxPath);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  JsonArrayConst entries = doc["approvals"].as<JsonArrayConst>();
  for (JsonObjectConst a : entries) {
    ApprovalEntry e;
    std::strncpy(e.approvalId, a["approval_id"] | "", sizeof(e.approvalId) - 1);
    std::strncpy(e.jobId, a["job_id"] | "", sizeof(e.jobId) - 1);
    const char* actionStr = a["action"] | "keep";
    if (std::strcmp(actionStr, "print") == 0) {
      e.action = ApprovalAction::Print;
    } else if (std::strcmp(actionStr, "delete") == 0) {
      e.action = ApprovalAction::Delete;
    } else {
      e.action = ApprovalAction::Keep;
    }
    e.createdAt = a["created_at"] | 0;
    e.synced = a["synced"] | false;
    if (e.approvalId[0] == '\0') continue;
    outbox.append(e);
  }
  return true;
}

bool saveApprovalOutbox(const ApprovalOutboxIndex& outbox) {
  JsonDocument doc;
  JsonArray entries = doc["approvals"].to<JsonArray>();
  for (size_t i = 0; i < outbox.count(); i++) {
    const ApprovalEntry& e = outbox.at(i);
    JsonObject a = entries.add<JsonObject>();
    a["approval_id"] = e.approvalId;
    a["job_id"] = e.jobId;
    a["action"] = approvalActionName(e.action);
    a["created_at"] = e.createdAt;
    a["synced"] = e.synced;
  }

  String out;
  serializeJson(doc, out);
  SdMan.ensureDirectoryExists("/system");
  return store::writeFileAtomic(kOutboxPath, out);
}

}  // namespace store
