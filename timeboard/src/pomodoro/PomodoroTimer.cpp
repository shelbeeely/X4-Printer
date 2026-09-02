#include "pomodoro/PomodoroTimer.h"

namespace pomodoro {

PomodoroTimer::PomodoroTimer() : PomodoroTimer(Config()) {}

PomodoroTimer::PomodoroTimer(Config config) : config_(config) {}

uint32_t PomodoroTimer::phaseDurationMs(Phase p) const {
  switch (p) {
    case Phase::Work:
      return config_.workMinutes * 60000u;
    case Phase::ShortBreak:
      return config_.shortBreakMinutes * 60000u;
    case Phase::LongBreak:
      return config_.longBreakMinutes * 60000u;
    case Phase::Idle:
      return 0;
  }
  return 0;
}

void PomodoroTimer::enterPhase(Phase p, uint32_t nowMs) {
  phase_ = p;
  paused_ = false;
  phaseStartMs_ = nowMs;
  pausedElapsedMs_ = 0;
}

void PomodoroTimer::start(uint32_t nowMs) {
  if (phase_ == Phase::Idle) {
    enterPhase(Phase::Work, nowMs);
    return;
  }
  if (paused_) {
    phaseStartMs_ = nowMs - pausedElapsedMs_;  // preserve elapsed-so-far
    paused_ = false;
  }
}

void PomodoroTimer::pause(uint32_t nowMs) {
  if (phase_ == Phase::Idle || paused_) return;
  pausedElapsedMs_ = nowMs - phaseStartMs_;
  paused_ = true;
}

void PomodoroTimer::reset() {
  phase_ = Phase::Idle;
  paused_ = false;
  phaseStartMs_ = 0;
  pausedElapsedMs_ = 0;
  completedWorkCycles_ = 0;
}

void PomodoroTimer::tick(uint32_t nowMs) {
  if (phase_ == Phase::Idle || paused_) return;
  uint32_t elapsed = nowMs - phaseStartMs_;
  if (elapsed < phaseDurationMs(phase_)) return;

  if (phase_ == Phase::Work) {
    completedWorkCycles_++;
    bool longBreakDue = (completedWorkCycles_ % config_.cyclesBeforeLongBreak) == 0;
    enterPhase(longBreakDue ? Phase::LongBreak : Phase::ShortBreak, nowMs);
  } else {
    enterPhase(Phase::Work, nowMs);
  }
}

uint32_t PomodoroTimer::remainingSeconds(uint32_t nowMs) const {
  if (phase_ == Phase::Idle) return 0;
  uint32_t elapsed = paused_ ? pausedElapsedMs_ : (nowMs - phaseStartMs_);
  uint32_t duration = phaseDurationMs(phase_);
  if (elapsed >= duration) return 0;
  return (duration - elapsed) / 1000u;
}

}  // namespace pomodoro
