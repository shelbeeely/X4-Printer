#pragma once
// One concrete calendar event occurrence (recurring events are expanded
// into individual occurrences inside the parser's lookahead window --
// see IcsParser.h). Ported from Free-Ink/wakeink's calendar/Event.h,
// trimmed to what the idle screen actually needs: this firmware shows the
// single next upcoming event, not a filtered/alarm-triggering agenda, so
// wakeink's meeting-link detection (hasLink/linkJoinable/urls), attendee
// RSVP tracking (SelfStatus), organizer, location, and description are
// all dropped -- along with the EventFilter.h dependency they existed
// for. Transient (heap String fields) by design: instances of this only
// ever live on the stack during one CalendarSync::syncCalendars() call
// within an existing wake's sync pass, never persisted or held across a
// render loop -- see CalendarCache.h for the fixed-size struct that
// actually survives past that call.

#include <Arduino.h>

namespace calendar {

enum class EventStatus : int8_t {
  TENTATIVE = 0,
  CONFIRMED = 1,
  CANCELLED = 2,
};

struct Event {
  String uid;
  String title;
  time_t start = 0;
  time_t end = 0;
  bool allDay = false;
  EventStatus status = EventStatus::CONFIRMED;
  uint8_t calIndex = 0;

  // RECURRENCE-ID handling: an override replaces the base occurrence whose
  // start equals recurrenceId (same uid).
  bool isOverride = false;
  time_t recurrenceId = 0;
};

}  // namespace calendar
