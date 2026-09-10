#include "ui/PomodoroUI.h"

#include <cstdio>
#include <ctime>

namespace ui {

namespace {

const char* phaseName(pomodoro::Phase phase) {
  switch (phase) {
    case pomodoro::Phase::Idle:
      return "Idle";
    case pomodoro::Phase::Work:
      return "Work";
    case pomodoro::Phase::Break:
      return "Break";
    case pomodoro::Phase::LongBreak:
      return "Long Break";
  }
  return "Idle";
}

void formatMinSec(uint32_t totalSeconds, char* out, size_t outLen) {
  std::snprintf(out, outLen, "%u:%02u", static_cast<unsigned>(totalSeconds / 60), static_cast<unsigned>(totalSeconds % 60));
}

pomodoro::PomodoroConfig configFromSettings(const config::AppSettingsData* settings) {
  pomodoro::PomodoroConfig cfg;
  if (settings == nullptr) return cfg;
  cfg.workMinutes = settings->pomodoroWorkMinutes;
  cfg.breakMinutes = settings->pomodoroBreakMinutes;
  cfg.longBreakMinutes = settings->pomodoroLongBreakMinutes;
  cfg.sessionsBeforeLongBreak = settings->pomodoroSessionsBeforeLongBreak;
  cfg.checkpointMinutes = settings->pomodoroCheckpointMinutes;
  return cfg;
}

}  // namespace

void pomodoroScreen(App::ScreenType& screen, InboxUiState& state) {
  screen.header("Pomodoro", "Checkpoint-driven, not a live tick");

  if (state.pomodoroSession == nullptr || !state.pomodoroSession->isActive()) {
    screen.popup("No active session. Press Start to begin a Work phase.");
  } else {
    const pomodoro::PomodoroSession& session = *state.pomodoroSession;
    time_t now = time(nullptr);
    char remaining[16];
    formatMinSec(session.remainingSeconds(now), remaining, sizeof(remaining));

    char body[128];
    std::snprintf(body, sizeof(body), "%s\n%s remaining\n\nCompleted work sessions: %u", phaseName(session.phase()),
                  remaining, static_cast<unsigned>(session.state().completedWorkSessions));
    screen.popup(body);
  }

  bool active = state.pomodoroSession != nullptr && state.pomodoroSession->isActive();
  const freeink::ui::FooterAction footer[] = {
      {.label = active ? "Stop" : "Start", .action = active ? kActionPomodoroStop : kActionPomodoroStart},
      {.label = "Back", .action = kActionPomodoroBack},
  };
  screen.footer(footer, 2);
}

void registerPomodoroActions(App& app, InboxUiState& state) {
  app.on(
      kActionOpenPomodoro,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::Pomodoro;
      },
      &state);

  app.on(
      kActionPomodoroBack,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::Timeline;
      },
      &state);

  app.on(
      kActionPomodoroStart,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.pomodoroSession == nullptr) return;
        s.pomodoroSession->start(time(nullptr), configFromSettings(s.appSettings));
        pomodoro::savePomodoroSession(*s.pomodoroSession);
      },
      &state);

  app.on(
      kActionPomodoroStop,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.pomodoroSession == nullptr) return;
        s.pomodoroSession->stop();
        pomodoro::savePomodoroSession(*s.pomodoroSession);
      },
      &state);
}

void settingsPomodoroTab(App::ScreenType& screen, InboxUiState& state) {
  if (state.appSettings == nullptr) return;
  const config::AppSettingsData& settings = *state.appSettings;
  char body[192];
  std::snprintf(body, sizeof(body), "Work: %u min\nBreak: %u min\nLong break: %u min (every %u sessions)\nCheckpoint: %u min",
                settings.pomodoroWorkMinutes, settings.pomodoroBreakMinutes, settings.pomodoroLongBreakMinutes,
                settings.pomodoroSessionsBeforeLongBreak, settings.pomodoroCheckpointMinutes);
  screen.popup(body);
}

}  // namespace ui
