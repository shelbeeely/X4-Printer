#include "testutil.h"
#include <cstdio>

#include "pomodoro/PomodoroTimer.h"

using pomodoro::Phase;
using pomodoro::PomodoroTimer;

int main() {
  PomodoroTimer::Config cfg;
  cfg.workMinutes = 1;        // 60,000 ms — kept small so the math below is easy to read
  cfg.shortBreakMinutes = 1;  // 60,000 ms
  cfg.longBreakMinutes = 2;   // 120,000 ms
  cfg.cyclesBeforeLongBreak = 2;
  PomodoroTimer timer(cfg);

  uint32_t now = 1000;
  CHECK(timer.phase() == Phase::Idle);
  CHECK(timer.remainingSeconds(now) == 0);

  timer.start(now);
  CHECK(timer.phase() == Phase::Work);
  CHECK(!timer.isPaused());
  CHECK(timer.remainingSeconds(now) == 60);

  // tick() before the phase duration elapses is a no-op.
  now += 30000;
  timer.tick(now);
  CHECK(timer.phase() == Phase::Work);
  CHECK(timer.remainingSeconds(now) == 30);

  // pause() freezes the countdown even as "now" keeps advancing.
  timer.pause(now);
  CHECK(timer.isPaused());
  now += 50000;
  timer.tick(now);  // still a no-op while paused
  CHECK(timer.phase() == Phase::Work);
  CHECK(timer.remainingSeconds(now) == 30);  // unchanged by the 50s that passed while paused

  // Resuming preserves the 30s already elapsed, not the 50s spent paused.
  timer.start(now);
  CHECK(!timer.isPaused());
  CHECK(timer.remainingSeconds(now) == 30);

  // Elapse the rest of the work phase -> first short break (not long yet:
  // cyclesBeforeLongBreak is 2).
  now += 30000;
  timer.tick(now);
  CHECK(timer.phase() == Phase::ShortBreak);
  CHECK(timer.completedWorkCycles() == 1);
  CHECK(timer.remainingSeconds(now) == 60);

  // Elapse the break -> back to Work.
  now += 60000;
  timer.tick(now);
  CHECK(timer.phase() == Phase::Work);

  // Elapse this second work cycle -> long break.
  now += 60000;
  timer.tick(now);
  CHECK(timer.phase() == Phase::LongBreak);
  CHECK(timer.completedWorkCycles() == 2);
  CHECK(timer.remainingSeconds(now) == 120);

  // reset() returns to Idle and clears the cycle count.
  timer.reset();
  CHECK(timer.phase() == Phase::Idle);
  CHECK(timer.completedWorkCycles() == 0);
  CHECK(!timer.isPaused());

  std::puts("PomodoroTimerTest OK");
  return 0;
}
