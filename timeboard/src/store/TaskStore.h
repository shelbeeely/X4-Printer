#pragma once
// Durable, atomic (write-then-rename) persistence for model::Schedule on
// the device's internal flash. This board has no SD slot (unlike the
// X4), so this uses LittleFS instead of the main X4-Printer firmware's
// SD-backed AtomicJsonFile — same write-then-rename discipline, applied
// to internal flash: a crash or power loss mid-write must never leave a
// corrupt/partial schedule on disk, so save() always writes to a temp
// path first and only renames it into place once the write fully
// succeeds.

#include "model/Schedule.h"

namespace store {

class TaskStore {
 public:
  // Mounts LittleFS, formatting it if unmountable (true on first boot
  // ever). Call once from setup() before load()/save().
  static bool begin();

  // False (schedule left empty) on first boot, or if the file is
  // missing/corrupt — normal states, not errors; main.cpp seeds a demo
  // schedule when this happens.
  static bool load(model::Schedule& out);

  static bool save(const model::Schedule& schedule);
};

}  // namespace store
