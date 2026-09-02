#include "ui/PomodoroView.h"

#include <cstdio>

namespace ui {

namespace {

const char* phaseLabel(pomodoro::Phase p) {
  switch (p) {
    case pomodoro::Phase::Work:
      return "FOCUS";
    case pomodoro::Phase::ShortBreak:
      return "BREAK";
    case pomodoro::Phase::LongBreak:
      return "LONG BREAK";
    case pomodoro::Phase::Idle:
      return "READY";
  }
  return "";
}

uint16_t phaseColor(pomodoro::Phase p) {
  switch (p) {
    case pomodoro::Phase::Work:
      return TFT_RED;
    case pomodoro::Phase::ShortBreak:
    case pomodoro::Phase::LongBreak:
      return TFT_DARKGREEN;
    case pomodoro::Phase::Idle:
      return TFT_DARKGREY;
  }
  return TFT_BLACK;
}

}  // namespace

void PomodoroView::draw(M5GFX& gfx, const pomodoro::PomodoroTimer& timer, uint32_t nowMs) const {
  const uint16_t bg = phaseColor(timer.phase());
  gfx.fillScreen(bg);
  gfx.setTextDatum(middle_center);
  gfx.setTextColor(TFT_WHITE, bg);

  gfx.setTextSize(1);
  gfx.drawString(phaseLabel(timer.phase()), gfx.width() / 2, gfx.height() / 3);

  const uint32_t remaining = timer.remainingSeconds(nowMs);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(remaining / 60), static_cast<unsigned>(remaining % 60));
  gfx.setTextSize(2);
  gfx.drawString(buf, gfx.width() / 2, gfx.height() * 2 / 3);

  if (timer.isPaused()) {
    gfx.setTextSize(1);
    gfx.drawString("PAUSED", gfx.width() / 2, gfx.height() - 12);
  }
}

}  // namespace ui
