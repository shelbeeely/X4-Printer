// SD-backed persistence for PomodoroSession (PomodoroSession.h holds the
// freestanding, host-tested state machine; this file is the thin
// Arduino/FreeInk glue that loads/saves it as JSON, mirroring
// store/JobStore.cpp's split).

#include <ArduinoJson.h>
#include <SDCardManager.h>

#include "pomodoro/PomodoroSession.h"
#include "store/AtomicJsonFile.h"

namespace pomodoro {

constexpr const char* kPomodoroStatePath = "/pomodoro/state.json";

// Loads /pomodoro/state.json into `session`. Returns false if the file is
// missing (normal -- no session has ever been started) or malformed
// (treated the same as missing -- start Idle rather than fail to boot).
bool loadPomodoroSession(PomodoroSession& session) {
  if (!SdMan.exists(kPomodoroStatePath)) return false;
  String raw = SdMan.readFile(kPomodoroStatePath);
  if (raw.isEmpty()) return false;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  PomodoroState s;
  uint8_t rawPhase = doc["phase"] | 0;
  // Defensive: a corrupt/out-of-range value (e.g. a hand-edited or
  // partially-written state.json) falls back to Idle rather than an
  // unrecognized Phase that transitionOnce()'s switch has no case for --
  // same "malformed -> treat as safe default" spirit as
  // PlannerStore.cpp's category parsing and CategoryStyle::styleFor()'s
  // out-of-range clamp.
  s.phase = rawPhase <= static_cast<uint8_t>(Phase::LongBreak) ? static_cast<Phase>(rawPhase) : Phase::Idle;
  s.phaseStart = static_cast<time_t>(doc["phase_start"] | 0);
  s.phaseEnd = static_cast<time_t>(doc["phase_end"] | 0);
  s.completedWorkSessions = doc["completed_work_sessions"] | 0;
  s.config.workMinutes = doc["work_minutes"] | 25;
  s.config.breakMinutes = doc["break_minutes"] | 5;
  s.config.longBreakMinutes = doc["long_break_minutes"] | 15;
  s.config.sessionsBeforeLongBreak = doc["sessions_before_long_break"] | 4;
  s.config.checkpointMinutes = doc["checkpoint_minutes"] | 5;
  session.setState(s);
  return true;
}

bool savePomodoroSession(const PomodoroSession& session) {
  const PomodoroState& s = session.state();
  JsonDocument doc;
  doc["phase"] = static_cast<uint8_t>(s.phase);
  doc["phase_start"] = static_cast<uint32_t>(s.phaseStart);
  doc["phase_end"] = static_cast<uint32_t>(s.phaseEnd);
  doc["completed_work_sessions"] = s.completedWorkSessions;
  doc["work_minutes"] = s.config.workMinutes;
  doc["break_minutes"] = s.config.breakMinutes;
  doc["long_break_minutes"] = s.config.longBreakMinutes;
  doc["sessions_before_long_break"] = s.config.sessionsBeforeLongBreak;
  doc["checkpoint_minutes"] = s.config.checkpointMinutes;

  String out;
  serializeJson(doc, out);
  SdMan.ensureDirectoryExists("/pomodoro");
  return store::writeFileAtomic(kPomodoroStatePath, out);
}

}  // namespace pomodoro
