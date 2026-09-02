#pragma once
// One glanceable "block" of the day: an icon (see ui/IconSet.h), a color
// (the same color fills its slice of the color-coded timeline —
// ui/TimelineView.h), a planned duration, and optional sub-steps for
// breaking a big task down into smaller pieces. Subtasks are a flat list,
// not a tree: one level of breakdown is enough to turn "clean room" into
// a few glanceable pieces without adding UI/storage complexity for depth
// nobody asked for.
//
// Fixed-capacity fields throughout (kMaxLabelLen, kMaxSubtasks), same
// style as the main X4-Printer firmware's store:: types — this device has
// no SD card (see store/TaskStore.h), so the whole schedule lives in a
// small, bounded amount of RAM and flash.

#include <cstdint>

namespace model {

constexpr uint8_t kMaxLabelLen = 24;  // includes NUL
constexpr uint8_t kMaxSubtasks = 6;

struct SubTask {
  char label[kMaxLabelLen] = {0};
  uint8_t iconId = 0;  // ui::IconId, stored raw so this header doesn't depend on ui/
  bool done = false;
};

struct Task {
  char label[kMaxLabelLen] = {0};
  uint8_t iconId = 0;
  uint16_t colorRgb565 = 0;  // native display color; also the timeline segment's fill color
  uint16_t plannedMinutes = 0;
  bool done = false;

  uint8_t subtaskCount = 0;
  SubTask subtasks[kMaxSubtasks];
};

}  // namespace model
