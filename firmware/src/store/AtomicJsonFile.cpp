#include "store/AtomicJsonFile.h"

#include <SDCardManager.h>

#include <cstring>

namespace store {

bool writeFileAtomic(const char* path, const String& content) {
  String tmpPath = String(path) + ".tmp";

  if (!SdMan.writeFile(tmpPath.c_str(), content)) {
    return false;
  }

  if (SdMan.exists(path)) {
    if (!SdMan.remove(path)) {
      SdMan.remove(tmpPath.c_str());
      return false;
    }
  }

  if (!SdMan.rename(tmpPath.c_str(), path)) {
    // Best-effort: leave the .tmp file in place rather than deleting
    // content that might still be recoverable by hand; the next load()
    // reads `path`, which no longer exists, so the store starts empty
    // (see header comment) — a subsequent successful save() overwrites
    // the stale .tmp on its next writeFile() call.
    return false;
  }

  return true;
}

}  // namespace store
