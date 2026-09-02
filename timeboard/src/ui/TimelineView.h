#pragma once
// Renders model::Schedule as a color-coded timeline strip filling the
// whole screen, in either horizontal (left->right = time) or vertical
// (top->bottom = time) orientation. Owns only the orientation
// preference; main.cpp passes in the live schedule and elapsed time each
// frame rather than this class holding a copy.

#include <M5GFX.h>

#include <cstdint>

#include "model/Schedule.h"
#include "orientation/Orientation.h"

namespace ui {

class TimelineView {
 public:
  explicit TimelineView(orientation::Mode mode = orientation::Mode::Horizontal) : mode_(mode) {}

  orientation::Mode mode() const { return mode_; }
  void setMode(orientation::Mode m) { mode_ = m; }
  void toggleMode() { mode_ = orientation::toggle(mode_); }

  // Fills the given gfx's whole current screen with one segment per task
  // (length proportional to plannedMinutes, filled with the task's
  // color, glyph centered), plus a contrasting "now" line at
  // elapsedMinutes into the schedule. No-op if the schedule is empty or
  // has zero total planned minutes (nothing to divide the screen by).
  void draw(M5GFX& gfx, const model::Schedule& schedule, uint32_t elapsedMinutes) const;
};

}  // namespace ui
