#pragma once
// V1 icon set: each icon is a short glyph (a word or two, e.g. "Eat",
// "Zzz") drawn centered on the task's own color — not real pictogram
// artwork like PictoStick's ~150 Material Design icons.
//
// This is a deliberate placeholder, not an oversight: converting a
// licensed icon pack (Material Icons, Apache-2.0) into RGB565 bitmaps for
// M5GFX, and picking a legible size/set, is real work that has no way to
// be visually verified without the physical M5StickC Plus2 hardware in
// this environment. glyphFor() is the one place that would change to
// swap in real bitmaps later — model::Task and ui::TimelineView only ever
// deal in an iconId, never in how it's drawn.

#include <cstdint>

namespace ui {

enum class IconId : uint8_t {
  Generic = 0,
  Meal,
  Sleep,
  Hygiene,
  School,
  Work,
  Exercise,
  Medication,
  Chore,
  Play,
  Travel,
  Break,
  Count
};

// Takes the raw iconId stored on model::Task/model::SubTask (kept as a
// plain uint8_t there so model/ doesn't need to depend on ui/) and
// returns the label to draw. Any value >= IconId::Count falls back to
// Generic's "?" rather than reading out of bounds.
const char* glyphFor(uint8_t iconId);

}  // namespace ui
