#pragma once
// Classic Pomodoro state machine: Work -> Break -> Work -> ..., with a
// longer break every Nth cycle. Pure logic, driven entirely by a caller-
// supplied "now" (millis()) rather than reading the clock itself, so it's
// host-testable (test/pomodoro/PomodoroTimerTest.cpp) the same way the
// main X4-Printer firmware's pure-logic modules are (firmware/test/).

#include <cstdint>

namespace pomodoro {

enum class Phase { Idle, Work, ShortBreak, LongBreak };

class PomodoroTimer {
 public:
  struct Config {
    uint32_t workMinutes = 25;
    uint32_t shortBreakMinutes = 5;
    uint32_t longBreakMinutes = 15;
    uint8_t cyclesBeforeLongBreak = 4;
  };

  // Two overloads rather than a `Config config = Config()` default
  // argument: GCC/Clang reject a default argument that default-
  // constructs a nested class from within the enclosing class body
  // (Config's own in-class member initializers aren't visible yet at
  // that point) — see PomodoroTimer.cpp.
  PomodoroTimer();
  explicit PomodoroTimer(Config config);

  // From Idle, starts a Work phase. From a paused phase, resumes it
  // (preserving elapsed time). No-op if already running unpaused —
  // deliberately not restart-on-start, so an accidental double-press
  // mid-session can't lose progress.
  void start(uint32_t nowMs);

  // Freezes the current phase's countdown. No-op if Idle or already
  // paused.
  void pause(uint32_t nowMs);

  // Returns to Idle and zeroes the completed-cycle count. The one true
  // "start over" — start()/pause() alone can never lose a running
  // session's progress, only this can.
  void reset();

  // Advances to the next phase if the current phase's duration has
  // elapsed. Call every loop() pass; cheap/no-op while Idle or paused.
  void tick(uint32_t nowMs);

  Phase phase() const { return phase_; }
  bool isPaused() const { return paused_; }
  uint32_t remainingSeconds(uint32_t nowMs) const;
  uint8_t completedWorkCycles() const { return completedWorkCycles_; }

 private:
  Config config_;
  Phase phase_ = Phase::Idle;
  bool paused_ = false;
  uint32_t phaseStartMs_ = 0;     // adjusted on resume so elapsed-so-far is preserved
  uint32_t pausedElapsedMs_ = 0;  // elapsed time within the current phase, snapshotted at pause()
  uint8_t completedWorkCycles_ = 0;

  uint32_t phaseDurationMs(Phase p) const;
  void enterPhase(Phase p, uint32_t nowMs);
};

}  // namespace pomodoro
