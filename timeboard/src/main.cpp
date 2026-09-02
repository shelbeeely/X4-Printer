// TimeBoard: a PictoStick-inspired visual time-planning device — see
// README.md for the full feature list and hardware. Two screens toggled
// by the power button: the color-coded Timeline (default) and a
// full-screen Pomodoro countdown; BtnB toggles horizontal/vertical
// timeline orientation; BtnA starts/pauses/holds-to-reset the Pomodoro
// timer.

#include <M5Unified.h>

#include <cstring>

#include "model/Schedule.h"
#include "orientation/Orientation.h"
#include "pomodoro/PomodoroTimer.h"
#include "store/TaskStore.h"
#include "ui/IconSet.h"
#include "ui/PomodoroView.h"
#include "ui/TimelineView.h"

namespace {

enum class ScreenMode { Timeline, Pomodoro };

model::Schedule schedule;
pomodoro::PomodoroTimer pomodoroTimer;
ui::TimelineView timelineView;
ui::PomodoroView pomodoroView;
ScreenMode screenMode = ScreenMode::Timeline;

// Stand-in for "schedule start" — this device has no RTC/NTP wall-clock
// sync yet (see README.md "Known limitations"), so "elapsed minutes into
// today's schedule" is really "elapsed minutes since this boot." Good
// enough to exercise the timeline/marker/Pomodoro UI; a real wall clock
// is a natural v2.
uint32_t scheduleStartMs = 0;

uint32_t elapsedScheduleMinutes() { return (millis() - scheduleStartMs) / 60000u; }

void addDemoTask(const char* label, ui::IconId icon, uint16_t color, uint16_t minutes) {
  model::Task t;
  std::strncpy(t.label, label, model::kMaxLabelLen - 1);
  t.iconId = static_cast<uint8_t>(icon);
  t.colorRgb565 = color;
  t.plannedMinutes = minutes;
  schedule.add(t);
}

// Seeds a plausible first-boot schedule so the screen is never blank
// before real tasks are configured — there's no on-device task editor
// yet (see README.md "Known limitations"); for now, tasks are edited by
// changing this function and reflashing, same "extension point, not
// wired up yet" honesty as the main X4-Printer firmware's WifiStore.
void seedDemoSchedule() {
  addDemoTask("Breakfast", ui::IconId::Meal, TFT_ORANGE, 30);
  addDemoTask("School", ui::IconId::School, TFT_BLUE, 180);
  addDemoTask("Lunch", ui::IconId::Meal, TFT_ORANGE, 30);
  addDemoTask("Homework", ui::IconId::Work, TFT_PURPLE, 60);
  addDemoTask("Play", ui::IconId::Play, TFT_GREEN, 60);
  addDemoTask("Dinner", ui::IconId::Meal, TFT_ORANGE, 30);
  addDemoTask("Bedtime", ui::IconId::Sleep, TFT_NAVY, 30);
}

void handleButtons(uint32_t nowMs) {
  if (M5.BtnB.wasClicked()) {
    timelineView.toggleMode();
  }

  if (M5.BtnA.wasHold()) {
    pomodoroTimer.reset();
  } else if (M5.BtnA.wasClicked()) {
    if (pomodoroTimer.phase() == pomodoro::Phase::Idle || pomodoroTimer.isPaused()) {
      pomodoroTimer.start(nowMs);
    } else {
      pomodoroTimer.pause(nowMs);
    }
  }

  if (M5.BtnPWR.wasClicked()) {
    // Focus lock (see docs in README.md "Distraction blocking"): leaving
    // an active, unpaused Work phase to browse the timeline auto-pauses
    // the session rather than letting a "quick peek" keep the countdown
    // running unattended, and rather than blocking the switch outright
    // with no way to check the schedule at all.
    bool leavingActiveWork =
        screenMode == ScreenMode::Pomodoro && pomodoroTimer.phase() == pomodoro::Phase::Work && !pomodoroTimer.isPaused();
    if (leavingActiveWork) {
      pomodoroTimer.pause(nowMs);
    }
    screenMode = (screenMode == ScreenMode::Timeline) ? ScreenMode::Pomodoro : ScreenMode::Timeline;
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);

  store::TaskStore::begin();
  if (!store::TaskStore::load(schedule) || schedule.count() == 0) {
    seedDemoSchedule();
    store::TaskStore::save(schedule);
  }
  scheduleStartMs = millis();
}

void loop() {
  M5.update();
  const uint32_t now = millis();

  pomodoroTimer.tick(now);
  handleButtons(now);

  M5.Display.startWrite();
  if (screenMode == ScreenMode::Timeline) {
    M5.Display.fillScreen(TFT_BLACK);
    timelineView.draw(M5.Display, schedule, elapsedScheduleMinutes());
  } else {
    pomodoroView.draw(M5.Display, pomodoroTimer, now);
  }
  M5.Display.endWrite();

  delay(100);
}
