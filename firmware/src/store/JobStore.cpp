// SD-backed persistence for JobIndex (JobStore.h holds the freestanding,
// host-tested capacity/lookup logic; this file is the thin Arduino/FreeInk
// glue that loads/saves it as JSON, following CrossPoint's PersistableStore
// pattern — see docs/architecture.md).

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>

#include "store/AtomicJsonFile.h"
#include "store/JobStore.h"

namespace store {

constexpr const char* kJobIndexPath = "/inbox/index.json";

// Loads /inbox/index.json into `index`. Returns false if the file is
// missing (normal on first boot) or malformed (treated the same as
// missing — start with an empty inbox rather than fail to boot).
bool loadJobIndex(JobIndex& index) {
  index.clear();
  if (!SdMan.exists(kJobIndexPath)) return false;
  String raw = SdMan.readFile(kJobIndexPath);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  JsonArrayConst jobs = doc["jobs"].as<JsonArrayConst>();
  for (JsonObjectConst j : jobs) {
    JobEntry e;
    std::strncpy(e.jobId, j["job_id"] | "", sizeof(e.jobId) - 1);
    std::strncpy(e.title, j["title"] | "", sizeof(e.title) - 1);
    std::strncpy(e.xtcPath, j["xtc_path"] | "", sizeof(e.xtcPath) - 1);
    e.xtcBytes = j["xtc_bytes"] | 0;
    std::strncpy(e.xtcSha256, j["xtc_sha256"] | "", sizeof(e.xtcSha256) - 1);
    e.pageCount = j["page_count"] | 0;
    e.createdAt = j["created_at"] | 0;
    e.status = static_cast<JobStatus>(uint8_t(j["status"] | 0));
    std::strncpy(e.landscapeXtcPath, j["landscape_xtc_path"] | "", sizeof(e.landscapeXtcPath) - 1);
    e.landscapeXtcBytes = j["landscape_xtc_bytes"] | 0;
    std::strncpy(e.landscapeXtcSha256, j["landscape_xtc_sha256"] | "", sizeof(e.landscapeXtcSha256) - 1);
    e.landscapePageCount = j["landscape_page_count"] | 0;
    if (e.jobId[0] == '\0') continue;
    index.upsert(e);  // never fails to grow past capacity here: the file was itself capacity-bounded when written
  }
  return true;
}

bool saveJobIndex(const JobIndex& index) {
  JsonDocument doc;
  JsonArray jobs = doc["jobs"].to<JsonArray>();
  for (size_t i = 0; i < index.count(); i++) {
    const JobEntry& e = index.at(i);
    JsonObject j = jobs.add<JsonObject>();
    j["job_id"] = e.jobId;
    j["title"] = e.title;
    j["xtc_path"] = e.xtcPath;
    j["xtc_bytes"] = e.xtcBytes;
    j["xtc_sha256"] = e.xtcSha256;
    j["page_count"] = e.pageCount;
    j["created_at"] = e.createdAt;
    j["status"] = static_cast<uint8_t>(e.status);
    j["landscape_xtc_path"] = e.landscapeXtcPath;
    j["landscape_xtc_bytes"] = e.landscapeXtcBytes;
    j["landscape_xtc_sha256"] = e.landscapeXtcSha256;
    j["landscape_page_count"] = e.landscapePageCount;
  }

  String out;
  serializeJson(doc, out);
  SdMan.ensureDirectoryExists("/inbox");
  return store::writeFileAtomic(kJobIndexPath, out);
}

}  // namespace store
