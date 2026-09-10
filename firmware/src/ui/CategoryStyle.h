#pragma once
// Category -> icon + dither-pattern mapping for the native (1bpp) Timeline
// and Pomodoro screens. Pure logic, host-testable: deliberately does NOT
// include FreeInkUI/Icon.h, so it stays compilable on the host test build
// without adding new lib_deps. A future UI unit wires an actual
// freeink::Icon per iconSlug by adding the Icons library to
// firmware/platformio.ini's lib_deps and running tools/gen_icons.py with a
// manifest line "<slug> = <slug>" per category -- that wiring is out of
// scope here.
//
// The on-device web UI's planner.html uses a *different* per-category
// coding (real CSS color, via docs/design-system.md tokens) since that
// surface isn't limited to 1bpp. The comment table below pairs each
// category with a suggested web token purely for cross-unit consistency;
// it's not consumed by any code in this file.

#include <cstdint>

#include "store/PlannerStore.h"

namespace ui {

// Mirrors the dither vocabulary FreeInkUIDisplayTarget.h's Bayer-threshold
// Color enum actually offers (Black/DarkGray/LightGray -- see that file),
// without including it:
//   Solid  -> freeink::ui::Color::Black     (bayerAt() ignored, 100% ink)
//   Dense  -> freeink::ui::Color::DarkGray  (bayerAt(x,y) < 12, ~75% ink)
//   Sparse -> freeink::ui::Color::LightGray (bayerAt(x,y) < 4, ~25% ink)
enum class DitherPattern : uint8_t { Solid, Dense, Sparse };

struct CategoryVisual {
  const char* iconSlug;  // Lucide SVG filename stem, e.g. "briefcase"
  DitherPattern dither;
};

// Category      iconSlug         DitherPattern  // web token (docs/design-system.md)
// Work          briefcase        Solid          // --accent  (mustard - primary/active)
// Break         coffee           Sparse         // --sage    (muted green - restful)
// Chore         spray-can        Dense          // --accent2 (brick-red - task-like)
// Health        heart-pulse      Solid          // --gold    (warm - vital)
// Social        users            Dense          // --accent  (mustard - shared w/ Work, icon differentiates)
// School        graduation-cap   Sparse         // --gold    (shared w/ Health, icon differentiates)
// Personal      user             Dense          // --sage    (shared w/ Break, icon differentiates)
// Other         circle-ellipsis  Sparse         // --accent2 (shared w/ Chore, icon differentiates)
//
// (Web-token pairing above is a non-binding suggestion for the web-UI
// unit, not a contract this header enforces.)
const CategoryVisual& styleFor(store::Category category);

}  // namespace ui
