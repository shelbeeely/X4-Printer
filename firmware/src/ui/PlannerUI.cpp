#include "ui/PlannerUI.h"

#include <cstdio>
#include <cstring>

#include "ui/CategoryStyle.h"
#include "ui/TimelineMerge.h"

namespace ui {

namespace {

freeink::ui::Color colorForDither(DitherPattern d) {
  switch (d) {
    case DitherPattern::Solid:
      return freeink::ui::Color::Black;
    case DitherPattern::Dense:
      return freeink::ui::Color::DarkGray;
    case DitherPattern::Sparse:
      return freeink::ui::Color::LightGray;
  }
  return freeink::ui::Color::Black;
}

// Vertical layout: a plain FreeInkUI list, one row per merged item --
// selection/paging, footer wiring, and text layout all come for free from
// the list component, the same way homeScreen()'s job list does. Each
// row's leading glyph is the icon+pattern coding's text stand-in (the
// Icons library isn't wired into this project's platformio.ini lib_deps
// yet -- see ui/CategoryStyle.h's header comment -- so a real bitmap icon
// per category is a follow-up, not this unit's scope); the subtitle names
// the category (or "Calendar" for the merged-in event) so the coding is
// legible even before real icons land.
void renderVertical(App::ScreenType& screen, const TimelineItem* items, size_t count) {
  static freeink::ui::ListItem listItems[kMaxTimelineItems];
  static char labels[kMaxTimelineItems][kTimelineLabelLen + 8];

  for (size_t i = 0; i < count; i++) {
    const char* prefix = items[i].isCalendarEvent ? "[C]" : (items[i].done ? "[x]" : "[ ]");
    std::snprintf(labels[i], sizeof(labels[i]), "%s %s", prefix, items[i].label);
    listItems[i] = freeink::ui::ListItem{};
    listItems[i].label = labels[i];
    listItems[i].subtitle = items[i].isCalendarEvent ? "Calendar" : store::categoryName(items[i].category);
    listItems[i].actionValue = static_cast<int32_t>(i);
  }

  static int16_t selected = 0;
  if (selected >= static_cast<int16_t>(count)) selected = 0;
  screen.list(listItems, count, selected, kActionTimelineRowSelect);
}

// Horizontal layout: a single left-to-right strip of fixed-width cells,
// custom-drawn via DrawTarget (fill/text) rather than FreeInkUI's list
// component, which only lays out vertically. Deliberately non-scrolling
// for this first pass (see ui/PlannerUI.h's header comment): shows as many
// cells as fit across the panel and a "+N more" trailer for the rest,
// rather than a custom directional-scroll input handler.
void renderHorizontal(App::ScreenType& screen, const TimelineItem* items, size_t count) {
  freeink::ui::Rect body = screen.body();
  if (body.empty() || count == 0) return;

  constexpr int16_t kCellWidth = 150;
  constexpr int16_t kCellGap = 8;
  constexpr int16_t kCellPadding = 6;
  size_t maxCells = static_cast<size_t>(body.width / (kCellWidth + kCellGap));
  if (maxCells == 0) maxCells = 1;
  bool truncated = count > maxCells;
  size_t shown = truncated ? maxCells - 1 : count;  // leave room for the "+N more" trailer cell when truncated
  if (!truncated) shown = count;

  int16_t x = body.x;
  for (size_t i = 0; i < shown; i++) {
    freeink::ui::Rect cell{x, body.y, kCellWidth, body.height};
    const CategoryVisual& visual = items[i].isCalendarEvent ? CategoryVisual{"calendar", DitherPattern::Dense}
                                                              : styleFor(items[i].category);
    freeink::ui::Color bg = colorForDither(visual.dither);
    freeink::ui::Color fg = (visual.dither == DitherPattern::Solid) ? freeink::ui::Color::White : freeink::ui::Color::Black;

    screen.target().fill(cell, freeink::ui::Paint::solid(bg), 6);
    screen.target().stroke(cell, freeink::ui::Paint::solid(freeink::ui::Color::Black), 1, 6);

    freeink::ui::Rect textRect{static_cast<int16_t>(cell.x + kCellPadding), static_cast<int16_t>(cell.y + kCellPadding),
                                static_cast<int16_t>(cell.width - 2 * kCellPadding),
                                static_cast<int16_t>(cell.height - 2 * kCellPadding)};
    freeink::ui::TextStyle style{};
    style.color = fg;
    style.maxLines = 6;
    style.align = freeink::ui::TextAlign::Left;
    char body_text[kTimelineLabelLen + 16];
    std::snprintf(body_text, sizeof(body_text), "%s\n%s", items[i].done ? "[done]" : "",
                  items[i].label);
    screen.target().text(textRect, body_text, style);

    x = static_cast<int16_t>(x + kCellWidth + kCellGap);
  }

  if (truncated) {
    freeink::ui::Rect trailer{x, body.y, kCellWidth, body.height};
    char more[32];
    std::snprintf(more, sizeof(more), "+%zu more", count - shown);
    freeink::ui::TextStyle style{};
    style.color = freeink::ui::Color::Black;
    style.align = freeink::ui::TextAlign::Center;
    screen.target().text(trailer, more, style);
  }
}

}  // namespace

void timelineScreen(App::ScreenType& screen, InboxUiState& state) {
  bool horizontal = state.appSettings != nullptr && state.appSettings->plannerHorizontalView;

  TimelineItem items[kMaxTimelineItems];
  size_t count = 0;
  if (state.plannerTasks != nullptr) {
    count = buildTimelineItems(*state.plannerTasks, state.nextEvent, items, kMaxTimelineItems);
  }

  char subtitle[32];
  std::snprintf(subtitle, sizeof(subtitle), "%s * %zu item%s", horizontal ? "Horizontal" : "Vertical", count,
                count == 1 ? "" : "s");
  screen.header("Timeline", subtitle);

  if (count == 0) {
    screen.popup(
        "Nothing scheduled today. Tasks are added from the Pi's admin console (docs/planner.md).");
  } else if (horizontal) {
    renderHorizontal(screen, items, count);
  } else {
    renderVertical(screen, items, count);
  }

  const freeink::ui::FooterAction footer[] = {
      {.label = horizontal ? "View: Vert" : "View: Horiz", .action = kActionTimelineToggleView},
      {.label = "Back", .action = kActionTimelineBack},
  };
  screen.footer(footer, 2);
}

void registerTimelineActions(App& app, InboxUiState& state) {
  app.on(
      kActionOpenTimeline,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::Timeline;
      },
      &state);

  app.on(
      kActionTimelineBack,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        static_cast<InboxUiState*>(userPtr)->mode = ScreenMode::Inbox;
      },
      &state);

  app.on(
      kActionTimelineToggleView,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.appSettings == nullptr) return;
        s.appSettings->plannerHorizontalView = !s.appSettings->plannerHorizontalView;
        config::AppSettings::instance().setPlannerHorizontalView(s.appSettings->plannerHorizontalView);
        config::AppSettings::instance().save();
      },
      &state);

  app.on(
      kActionSettingsPlannerRowSelect,
      [](const freeink::ui::ActionEvent&, void* userPtr) {
        auto& s = *static_cast<InboxUiState*>(userPtr);
        if (s.appSettings == nullptr) return;
        s.appSettings->plannerHorizontalView = !s.appSettings->plannerHorizontalView;
        config::AppSettings::instance().setPlannerHorizontalView(s.appSettings->plannerHorizontalView);
        config::AppSettings::instance().save();
      },
      &state);

  app.on(
      kActionTimelineRowSelect,
      [](const freeink::ui::ActionEvent&, void*) {
        // No-op: read-only for now, see this file's/PlannerUI.h's header
        // comment. Registered so the list stays focusable/navigable.
      },
      &state);
}

void settingsPlannerTab(App::ScreenType& screen, InboxUiState& state) {
  if (state.appSettings == nullptr) return;
  freeink::ui::ListItem item{};
  item.label = "Timeline orientation";
  item.value = state.appSettings->plannerHorizontalView ? "Horizontal" : "Vertical";
  item.toggle = true;
  item.toggleChecked = state.appSettings->plannerHorizontalView;
  item.actionValue = 0;
  screen.list(&item, 1, 0, kActionSettingsPlannerRowSelect);
}

}  // namespace ui
