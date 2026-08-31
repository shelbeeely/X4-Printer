#include "calendar/CalendarSync.h"

#include <Arduino.h>

#include <cstring>
#include <ctime>
#include <vector>

#include "calendar/Event.h"
#include "calendar/IcsParser.h"
#include "config/CalendarCache.h"
#include "net/IcsFetch.h"

namespace calendar {

namespace {

constexpr time_t kLookaheadSeconds = 60 * 86400;  // 60 days
// Before this, the device's clock clearly hasn't been NTP-synced yet this
// wake (see sync/SyncManager.cpp's syncClock()) -- computing RRULE windows
// against an unset clock would produce nonsense, so skip the whole pass
// rather than cache a wrong "next event".
constexpr time_t kMinPlausibleNow = 1700000000;  // 2023-11-14

}  // namespace

void syncCalendars(const config::CalendarConfig& cfg) {
  if (cfg.count() == 0) return;

  const time_t now = time(nullptr);
  if (now < kMinPlausibleNow) return;

  const time_t windowEnd = now + kLookaheadSeconds;

  std::vector<Event> events;
  bool anyFeedOk = false;

  for (size_t i = 0; i < cfg.count(); i++) {
    const config::CalendarFeed& feed = cfg.at(i);
    if (feed.url[0] == '\0') continue;

    IcsParser parser(events, static_cast<uint8_t>(i), now, windowEnd);
    net::IcsFetchResult result = net::fetchIcs(String(feed.url), parser);
    if (result == net::IcsFetchResult::Ok) anyFeedOk = true;
  }

  if (!anyFeedOk) return;  // leave the existing cache untouched -- see header comment

  // Soonest event that hasn't ended yet (covers one currently in progress
  // as well as future ones) -- CANCELLED occurrences never reach `events`
  // (IcsParser::endEvent/finish already drop them).
  const Event* soonest = nullptr;
  for (const Event& ev : events) {
    if (ev.end <= now) continue;
    if (soonest == nullptr || ev.start < soonest->start) soonest = &ev;
  }

  config::NextEventInfo info;
  if (soonest != nullptr) {
    info.hasEvent = true;
    std::strncpy(info.title, soonest->title.c_str(), sizeof(info.title) - 1);
    info.start = soonest->start;
    info.end = soonest->end;
    info.allDay = soonest->allDay;
  }
  info.lastSyncedAt = now;
  // Carried forward unchanged, not recomputed here -- see
  // NextEventInfo::alertedForStart/alertedForEnd's comment in
  // CalendarCache.h. calendar::computeWakeDecision (WakeSchedule.h) is what
  // actually stamps these when a reminder fires; if the soonest event
  // changed since the last sync, its stale target simply won't match the
  // new event's start/end and the reminder logic re-arms itself with no
  // special-casing needed here.
  const config::NextEventInfo& previous = config::CalendarCache::instance().data();
  info.alertedForStart = previous.alertedForStart;
  info.alertedForEnd = previous.alertedForEnd;

  config::CalendarCache::instance().set(info);
  config::CalendarCache::instance().save();
}

}  // namespace calendar
