#include "ui/TimelineView.h"

#include "ui/IconSet.h"

namespace ui {

void TimelineView::draw(M5GFX& gfx, const model::Schedule& schedule, uint32_t elapsedMinutes) const {
  const uint32_t total = schedule.totalPlannedMinutes();
  if (schedule.count() == 0 || total == 0) return;

  const int32_t w = gfx.width();
  const int32_t h = gfx.height();
  const bool horizontal = (mode_ == orientation::Mode::Horizontal);
  const int32_t longAxis = horizontal ? w : h;

  gfx.setTextDatum(middle_center);

  int32_t offset = 0;
  for (uint8_t i = 0; i < schedule.count(); i++) {
    const model::Task& t = schedule.at(i);
    int32_t segLen = static_cast<int32_t>((static_cast<uint64_t>(t.plannedMinutes) * longAxis) / total);
    if (i == schedule.count() - 1) segLen = longAxis - offset;  // last segment absorbs rounding

    int32_t segCenter = offset + segLen / 2;
    if (horizontal) {
      gfx.fillRect(offset, 0, segLen, h, t.colorRgb565);
      gfx.setTextColor(TFT_BLACK, t.colorRgb565);
      gfx.drawString(glyphFor(t.iconId), segCenter, h / 2);
    } else {
      gfx.fillRect(0, offset, w, segLen, t.colorRgb565);
      gfx.setTextColor(TFT_BLACK, t.colorRgb565);
      gfx.drawString(glyphFor(t.iconId), w / 2, segCenter);
    }

    offset += segLen;
  }

  // "now" marker: a 2px contrasting line at elapsedMinutes' position.
  int32_t markerPos = static_cast<int32_t>((static_cast<uint64_t>(elapsedMinutes) * longAxis) / total);
  if (markerPos > longAxis - 1) markerPos = longAxis - 1;
  if (horizontal) {
    gfx.drawFastVLine(markerPos, 0, h, TFT_WHITE);
    if (markerPos > 0) gfx.drawFastVLine(markerPos - 1, 0, h, TFT_WHITE);
  } else {
    gfx.drawFastHLine(0, markerPos, w, TFT_WHITE);
    if (markerPos > 0) gfx.drawFastHLine(0, markerPos - 1, w, TFT_WHITE);
  }
}

}  // namespace ui
