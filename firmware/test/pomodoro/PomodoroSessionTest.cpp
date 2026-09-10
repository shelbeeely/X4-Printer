#include "testutil.h"
#include <cstdio>

#include "pomodoro/PomodoroSession.h"

using pomodoro::Phase;
using pomodoro::PomodoroConfig;
using pomodoro::PomodoroSession;
using pomodoro::PomodoroState;

namespace {

constexpr time_t kBase = 1700000000;  // arbitrary fixed epoch for reproducible tests

PomodoroConfig defaultConfig() { return PomodoroConfig{}; }  // 25/5/15/4/5

}  // namespace

int main() {
  // 1. start() sets Work phase, correct phaseEnd, completedWorkSessions == 0.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();
    s.start(kBase, cfg);
    CHECK(s.phase() == Phase::Work);
    CHECK(s.isActive());
    CHECK(s.phaseEnd() == kBase + 25 * 60);
    CHECK(s.state().completedWorkSessions == 0);
  }

  // 2. Exact-boundary transition: advance(phaseEnd) transitions, advance(phaseEnd-1) does not.
  {
    PomodoroSession s;
    s.start(kBase, defaultConfig());
    time_t end = s.phaseEnd();
    CHECK(s.advance(end - 1) == false);
    CHECK(s.phase() == Phase::Work);
    CHECK(s.advance(end) == true);
    CHECK(s.phase() == Phase::Break);
  }

  // 3. Work -> Break -> Work cycle for sessions 1..3, then session 4 goes Work -> LongBreak.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();  // sessionsBeforeLongBreak = 4
    s.start(kBase, cfg);
    for (int i = 0; i < 3; i++) {
      CHECK(s.phase() == Phase::Work);
      s.advance(s.phaseEnd());
      CHECK(s.phase() == Phase::Break);
      s.advance(s.phaseEnd());
    }
    CHECK(s.phase() == Phase::Work);
    CHECK(s.state().completedWorkSessions == 3);
    s.advance(s.phaseEnd());  // 4th work session completes
    CHECK(s.phase() == Phase::LongBreak);
    CHECK(s.state().completedWorkSessions == 4);
  }

  // 4. LongBreak -> Work resets completedWorkSessions to 0; next cycle repeats correctly.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();
    s.start(kBase, cfg);
    for (int i = 0; i < 4; i++) {
      s.advance(s.phaseEnd());  // Work -> Break/LongBreak
      s.advance(s.phaseEnd());  // Break/LongBreak -> Work
    }
    CHECK(s.phase() == Phase::Work);
    CHECK(s.state().completedWorkSessions == 0);
    // Session 5 total is "session 1 of new cycle" -- should go to Break, not LongBreak.
    s.advance(s.phaseEnd());
    CHECK(s.phase() == Phase::Break);
    CHECK(s.state().completedWorkSessions == 1);
  }

  // 5. Multi-phase catch-up: advance() once far past several boundaries.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();
    s.start(kBase, cfg);
    // Work(25) -> Break(5) -> Work(25) -> ... sleep through Work+Break+Work = 55 min.
    time_t farFuture = kBase + (25 + 5 + 25) * 60 + 30;  // 30s into the following Break
    CHECK(s.advance(farFuture) == true);
    CHECK(s.phase() == Phase::Break);
    CHECK(s.state().completedWorkSessions == 2);
    // phaseStart should be exactly at the second Work's end (25+5+25 min after kBase).
    time_t expectedPhaseStart = kBase + (25 + 5 + 25) * 60;
    CHECK(s.state().phaseStart == expectedPhaseStart);
    CHECK(s.phaseEnd() == expectedPhaseStart + 5 * 60);
  }

  // 6. stop() from any phase -> Idle; secondsUntilNextCheckpoint() returns nullopt when Idle.
  {
    PomodoroSession s;
    s.start(kBase, defaultConfig());
    s.advance(s.phaseEnd());  // now in Break
    s.stop();
    CHECK(s.phase() == Phase::Idle);
    CHECK(!s.isActive());
    CHECK(s.secondsUntilNextCheckpoint(kBase) == std::nullopt);
    CHECK(s.advance(kBase + 100000) == false);
  }

  // 7. Checkpoint calc, interval evenly dividing phase length (25/5): checkpoints at +5,10,...,25.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();  // workMinutes=25, checkpointMinutes=5
    s.start(kBase, cfg);
    CHECK(*s.secondsUntilNextCheckpoint(kBase) == 5 * 60);
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 5 * 60) == 5 * 60);       // exactly on a boundary -> next one
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 5 * 60 + 30) == 5 * 60 - 30);  // next boundary is +10min, 4:30 away
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 20 * 60) == 5 * 60);      // last checkpoint == phase end
  }

  // 8. Checkpoint calc, interval NOT evenly dividing (25 min phase, 7 min checkpoint):
  //    boundaries at +7,14,21, then final wake at +25 (phase end), never past it.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();
    cfg.checkpointMinutes = 7;
    s.start(kBase, cfg);
    CHECK(*s.secondsUntilNextCheckpoint(kBase) == 7 * 60);
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 7 * 60) == 7 * 60);   // -> +14
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 14 * 60) == 7 * 60);  // -> +21
    // At +21, the next 7-min boundary would be +28, past phaseEnd (25) -- clamp to phaseEnd.
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 21 * 60) == 4 * 60);
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 24 * 60) == 60);
  }

  // 9. checkpointMinutes == 0 -> falls back to "wake only at phase end".
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();
    cfg.checkpointMinutes = 0;
    s.start(kBase, cfg);
    CHECK(*s.secondsUntilNextCheckpoint(kBase) == 25 * 60);
    CHECK(*s.secondsUntilNextCheckpoint(kBase + 10 * 60) == 15 * 60);
  }

  // 10. now already at/past a checkpoint boundary or past phaseEnd -> returns 0.
  {
    PomodoroSession s;
    s.start(kBase, defaultConfig());
    CHECK(*s.secondsUntilNextCheckpoint(s.phaseEnd()) == 0);
    CHECK(*s.secondsUntilNextCheckpoint(s.phaseEnd() + 1000) == 0);
    CHECK(s.remainingSeconds(s.phaseEnd()) == 0);
    CHECK(s.remainingSeconds(s.phaseEnd() + 1000) == 0);
  }

  // 11. Config with non-default values (sessionsBeforeLongBreak = 1) still cycles correctly.
  {
    PomodoroSession s;
    PomodoroConfig cfg = defaultConfig();
    cfg.sessionsBeforeLongBreak = 1;
    s.start(kBase, cfg);
    CHECK(s.phase() == Phase::Work);
    s.advance(s.phaseEnd());
    CHECK(s.phase() == Phase::LongBreak);  // every Work -> LongBreak
    s.advance(s.phaseEnd());
    CHECK(s.phase() == Phase::Work);
    CHECK(s.state().completedWorkSessions == 0);
    s.advance(s.phaseEnd());
    CHECK(s.phase() == Phase::LongBreak);
  }

  // 12. Round-trip through PomodoroState / setState() / state().
  {
    PomodoroSession s;
    PomodoroState custom;
    custom.phase = Phase::Break;
    custom.phaseStart = kBase;
    custom.phaseEnd = kBase + 300;
    custom.completedWorkSessions = 2;
    custom.config.workMinutes = 30;
    custom.config.breakMinutes = 6;
    custom.config.longBreakMinutes = 20;
    custom.config.sessionsBeforeLongBreak = 3;
    custom.config.checkpointMinutes = 2;
    s.setState(custom);
    CHECK(s.state().phase == Phase::Break);
    CHECK(s.state().phaseStart == kBase);
    CHECK(s.state().phaseEnd == kBase + 300);
    CHECK(s.state().completedWorkSessions == 2);
    CHECK(s.state().config.workMinutes == 30);
    CHECK(s.state().config.breakMinutes == 6);
    CHECK(s.state().config.longBreakMinutes == 20);
    CHECK(s.state().config.sessionsBeforeLongBreak == 3);
    CHECK(s.state().config.checkpointMinutes == 2);
  }

  std::printf("PomodoroSessionTest: all assertions passed\n");
  return 0;
}
