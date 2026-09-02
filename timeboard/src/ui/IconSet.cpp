#include "ui/IconSet.h"

namespace ui {

const char* glyphFor(uint8_t iconId) {
  if (iconId >= static_cast<uint8_t>(IconId::Count)) return "?";
  switch (static_cast<IconId>(iconId)) {
    case IconId::Meal:
      return "Eat";
    case IconId::Sleep:
      return "Zzz";
    case IconId::Hygiene:
      return "Wash";
    case IconId::School:
      return "Study";
    case IconId::Work:
      return "Work";
    case IconId::Exercise:
      return "Move";
    case IconId::Medication:
      return "Meds";
    case IconId::Chore:
      return "Chore";
    case IconId::Play:
      return "Play";
    case IconId::Travel:
      return "Go";
    case IconId::Break:
      return "Rest";
    case IconId::Generic:
    case IconId::Count:
      break;
  }
  return "?";
}

}  // namespace ui
