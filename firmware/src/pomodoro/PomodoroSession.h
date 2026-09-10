#pragma once
// Pomodoro session state machine -- freestanding, host-testable (no
// Arduino/FreeInk deps), same split rationale as store/JobStore.h. The
// device's whole design center is deep sleep with wake only on button
// press or its own RTC timer (docs/architecture.md "Deep sleep / wake
// sequence") -- there is no live-ticking display. A Pomodoro session
// therefore uses "checkpoint wakes": secondsUntilNextCheckpoint() below is
// a pure function meant to be folded into main.cpp's
// nextWakeIntervalSeconds() (see that method's own comment), alongside the
// existing calendar::computeWakeDecision() result, the same way that
// function already folds in calendar-driven near-wakes.

#include <cstdint>
#include <ctime>
#include <optional>

namespace pomodoro {

enum class Phase : uint8_t { Idle = 0, Work = 1, Break = 2, LongBreak = 3 };

struct PomodoroConfig {
  uint16_t workMinutes = 25;
  uint16_t breakMinutes = 5;
  uint16_t longBreakMinutes = 15;
  uint16_t sessionsBeforeLongBreak = 4;
  uint16_t checkpointMinutes = 5;
};

struct PomodoroState {
  Phase phase = Phase::Idle;
  time_t phaseStart = 0;
  time_t phaseEnd = 0;
  uint16_t completedWorkSessions = 0;  // count since the last LongBreak
  PomodoroConfig config;
};

class PomodoroSession {
 public:
  // Starts a fresh Work phase at `now` with `config`; resets
  // completedWorkSessions to 0 (a brand-new session always starts the
  // long-break cycle over).
  void start(time_t now, const PomodoroConfig& config) {
    state_.phase = Phase::Work;
    state_.phaseStart = now;
    state_.phaseEnd = now + durationSeconds(config.workMinutes);
    state_.completedWorkSessions = 0;
    state_.config = config;
  }

  // Cancels any active session -> Idle. Idempotent.
  void stop() {
    state_.phase = Phase::Idle;
    state_.phaseStart = 0;
    state_.phaseEnd = 0;
  }

  bool isActive() const { return state_.phase != Phase::Idle; }
  Phase phase() const { return state_.phase; }
  time_t phaseEnd() const { return state_.phaseEnd; }

  // Advances phase-by-phase while now >= phaseEnd, so a device that slept
  // through more than one phase boundary (checkpoint wake missed, long
  // idle) catches up in one call. Bounded iteration count guards against a
  // misconfigured 0-minute phase spinning forever. Returns true if at
  // least one transition happened.
  bool advance(time_t now) {
    if (state_.phase == Phase::Idle) return false;
    bool transitioned = false;
    constexpr int kMaxIterations = 1000;  // generous upper bound on catch-up transitions in one call
    for (int i = 0; i < kMaxIterations && now >= state_.phaseEnd; i++) {
      transitionOnce();
      transitioned = true;
    }
    return transitioned;
  }

  // Seconds remaining in the current phase; 0 if already due or Idle.
  uint32_t remainingSeconds(time_t now) const {
    if (state_.phase == Phase::Idle) return 0;
    if (now >= state_.phaseEnd) return 0;
    return static_cast<uint32_t>(state_.phaseEnd - now);
  }

  // ---- Integration point for the Pomodoro UI unit (main.cpp) ----
  // Pure function: seconds until this session should next wake the device
  // to redraw -- the sooner of the next checkpoint-interval boundary
  // (measured from phaseStart, so checkpoints that don't evenly divide the
  // phase length still land on a real boundary and a final wake exactly at
  // phaseEnd) and the phase's own end. Returns std::nullopt when no
  // session is active (Idle) -- caller should skip folding it into their
  // min(). Returns 0 when the phase (or a checkpoint boundary) is already
  // due "now" -- caller should treat that as "wake immediately", not skip.
  //
  // main.cpp's nextWakeIntervalSeconds() should compose this in alongside
  // calendar::computeWakeDecision()'s result, e.g.:
  //
  //   uint32_t nextWakeIntervalSeconds() {
  //     ...
  //     uint32_t sleepSeconds = decision.sleepSeconds;
  //     if (auto pomo = pomodoroSession.secondsUntilNextCheckpoint(time(nullptr))) {
  //       sleepSeconds = std::min(sleepSeconds, *pomo);
  //     }
  //     return sleepSeconds;
  //   }
  //
  // Deliberately takes `now` as a parameter (never reads time(nullptr)
  // internally) for the same host-testability reason as
  // calendar::computeWakeDecision().
  std::optional<uint32_t> secondsUntilNextCheckpoint(time_t now) const {
    if (state_.phase == Phase::Idle) return std::nullopt;
    if (now >= state_.phaseEnd) return 0;
    if (state_.config.checkpointMinutes == 0) {
      return static_cast<uint32_t>(state_.phaseEnd - now);
    }
    time_t checkpointSeconds = static_cast<time_t>(state_.config.checkpointMinutes) * 60;
    time_t elapsed = now > state_.phaseStart ? now - state_.phaseStart : 0;
    time_t nextBoundary = state_.phaseStart + ((elapsed / checkpointSeconds) + 1) * checkpointSeconds;
    time_t wakeAt = nextBoundary < state_.phaseEnd ? nextBoundary : state_.phaseEnd;
    if (wakeAt <= now) return 0;
    return static_cast<uint32_t>(wakeAt - now);
  }

  const PomodoroState& state() const { return state_; }
  void setState(const PomodoroState& s) { state_ = s; }  // for load()

 private:
  static time_t durationSeconds(uint16_t minutes) {
    // Guard 0-minute config against a zero-length (or negative-progress)
    // phase, on top of advance()'s own iteration cap.
    return static_cast<time_t>(minutes > 0 ? minutes : 1) * 60;
  }

  void transitionOnce() {
    time_t oldEnd = state_.phaseEnd;
    switch (state_.phase) {
      case Phase::Idle:
        return;  // unreachable: advance() already checks this
      case Phase::Work: {
        state_.completedWorkSessions++;
        bool timeForLongBreak =
            state_.config.sessionsBeforeLongBreak > 0 &&
            state_.completedWorkSessions % state_.config.sessionsBeforeLongBreak == 0;
        if (timeForLongBreak) {
          state_.phase = Phase::LongBreak;
          state_.phaseStart = oldEnd;
          state_.phaseEnd = oldEnd + durationSeconds(state_.config.longBreakMinutes);
        } else {
          state_.phase = Phase::Break;
          state_.phaseStart = oldEnd;
          state_.phaseEnd = oldEnd + durationSeconds(state_.config.breakMinutes);
        }
        return;
      }
      case Phase::Break:
        state_.phase = Phase::Work;
        state_.phaseStart = oldEnd;
        state_.phaseEnd = oldEnd + durationSeconds(state_.config.workMinutes);
        return;
      case Phase::LongBreak:
        state_.completedWorkSessions = 0;
        state_.phase = Phase::Work;
        state_.phaseStart = oldEnd;
        state_.phaseEnd = oldEnd + durationSeconds(state_.config.workMinutes);
        return;
    }
  }

  PomodoroState state_;
};

// SD persistence (PomodoroSession.cpp -- Arduino/FreeInk dependent, not
// part of the host-testable surface above). Path: /pomodoro/state.json.
bool loadPomodoroSession(PomodoroSession& session);
bool savePomodoroSession(const PomodoroSession& session);

}  // namespace pomodoro
