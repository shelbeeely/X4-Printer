#pragma once
// Pomodoro screen: shows pomodoro::PomodoroSession's current phase and
// remaining time, with Start/Stop actions. Reached from the Timeline
// screen's footer (ui/PlannerUI.h) rather than a 6th Inbox footer button,
// to avoid crowding that footer further after Unit 2 already added a 5th
// button there.
//
// Remaining time is coarse and checkpoint-driven, NOT a live per-second
// tick -- this device wakes only on button press or its own RTC timer
// (docs/architecture.md "Deep sleep / wake sequence"), and a Pomodoro
// timer gets no exception to that. See pomodoro/PomodoroSession.h's
// secondsUntilNextCheckpoint() and docs/planner.md for the full model.

#include "ui/InboxUI.h"

namespace ui {

constexpr freeink::ui::ActionId kActionOpenPomodoro = 110;
constexpr freeink::ui::ActionId kActionPomodoroBack = 111;
constexpr freeink::ui::ActionId kActionPomodoroStart = 112;
constexpr freeink::ui::ActionId kActionPomodoroStop = 113;

void pomodoroScreen(App::ScreenType& screen, InboxUiState& state);

// Registers every Pomodoro-related action handler on `app` -- called once
// from InboxUI.cpp's initApp(), alongside registerTimelineActions().
void registerPomodoroActions(App& app, InboxUiState& state);

// Settings > Pomodoro tab: read-only display of the configured durations
// (editing happens from the Pi admin console -- see AppSettings.h's
// pomodoro* fields' own comment for why).
void settingsPomodoroTab(App::ScreenType& screen, InboxUiState& state);

}  // namespace ui
