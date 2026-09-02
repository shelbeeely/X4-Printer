#pragma once
// Full-screen Pomodoro display: current phase name, mm:ss countdown, and
// a background color that changes between focus (work) and break so the
// phase is readable at a glance without reading the text.

#include <M5GFX.h>

#include <cstdint>

#include "pomodoro/PomodoroTimer.h"

namespace ui {

class PomodoroView {
 public:
  void draw(M5GFX& gfx, const pomodoro::PomodoroTimer& timer, uint32_t nowMs) const;
};

}  // namespace ui
