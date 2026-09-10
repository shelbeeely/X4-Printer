#pragma once
// Timeline screen: the day's user-authored planner tasks (store::TaskIndex,
// wired via InboxUiState::plannerTasks) merged with the existing calendar
// module's cached next event (InboxUiState::nextEvent), rendered vertically
// (a day-planner ruler, default) or horizontally (a single left-to-right
// strip) per config::AppSettingsData::plannerHorizontalView -- see
// docs/planner.md for the full feature writeup, including why this native
// screen uses icon+pattern coding rather than color (the panel is 1bpp).
//
// Read-only for now: this screen shows the merged timeline but doesn't yet
// offer marking a task done on-device (that needs a sync-back path this
// unit doesn't wire up -- see docs/protocol.md §1.8's completion endpoint,
// which the Pi/sync-glue side already supports). Horizontal mode is also
// non-scrolling in this first pass: it shows as many items as fit across
// the panel width and a "+N more" indicator for the rest, rather than a
// custom directional-scroll input handler -- flagged here as a known,
// deliberate scope reduction, not an oversight.

#include "ui/InboxUI.h"

namespace ui {

// Public action ids so InboxUI.cpp's homeScreen() footer and
// settingsScreen() can reference them without pulling in PlannerUI.cpp's
// internal action-id enum. Numbered from 100 to stay clear of InboxUI.cpp's
// own 1-21 range on the same App's action table (registerTimelineActions()
// below and homeScreen()'s footer array both reference these same values,
// so they must never collide with another module's ids on this app
// instance -- Unit 4's Pomodoro screen picks its own range above this one
// for the same reason).
constexpr freeink::ui::ActionId kActionOpenTimeline = 100;
constexpr freeink::ui::ActionId kActionTimelineBack = 101;
constexpr freeink::ui::ActionId kActionTimelineToggleView = 102;
constexpr freeink::ui::ActionId kActionSettingsPlannerRowSelect = 103;
// Row selection in the vertical list is currently a no-op -- this screen
// is read-only (see this header's top comment on why on-device task
// completion isn't wired up yet) -- registered anyway so the list is
// still focusable/navigable rather than firing an unregistered action.
constexpr freeink::ui::ActionId kActionTimelineRowSelect = 104;

void timelineScreen(App::ScreenType& screen, InboxUiState& state);

// Registers every Timeline-related action handler on `app` -- called once
// from InboxUI.cpp's initApp(), alongside its own app.on(...) calls.
void registerTimelineActions(App& app, InboxUiState& state);

// Settings > Planner tab (orientation toggle) -- rendered from
// InboxUI.cpp's settingsScreen() the same way settingsDisplayTab()/
// settingsCalendarTab() are, kept here instead since it's specific to this
// feature's own setting.
void settingsPlannerTab(App::ScreenType& screen, InboxUiState& state);

}  // namespace ui
