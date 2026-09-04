#include "ui/CategoryStyle.h"

namespace ui {

namespace {

// Indexed by static_cast<uint8_t>(store::Category) -- order must match
// store::Category's declaration exactly (PlannerStore.h says don't
// reorder that enum; this array is why).
constexpr CategoryVisual kStyles[8] = {
    {"briefcase", DitherPattern::Solid},        // Work
    {"coffee", DitherPattern::Sparse},          // Break
    {"spray-can", DitherPattern::Dense},        // Chore
    {"heart-pulse", DitherPattern::Solid},      // Health
    {"users", DitherPattern::Dense},            // Social
    {"graduation-cap", DitherPattern::Sparse},  // School
    {"user", DitherPattern::Dense},             // Personal
    {"circle-ellipsis", DitherPattern::Sparse}, // Other
};

}  // namespace

const CategoryVisual& styleFor(store::Category category) {
  uint8_t idx = static_cast<uint8_t>(category);
  // Defensive: a corrupt/out-of-range value (e.g. a malformed SD file that
  // slipped past parseCategoryName's default) clamps to Other's entry
  // rather than reading out of bounds -- same defensive spirit as
  // JobStore.cpp's "malformed file -> treat as missing".
  if (idx >= 8) idx = static_cast<uint8_t>(store::Category::Other);
  return kStyles[idx];
}

}  // namespace ui
