#pragma once
// Orchestrates a calendar sync pass: fetch every configured ICS feed,
// find the single soonest event across all of them, and persist it.
// Called from SyncManager::runFullSync() while Wi-Fi is already connected
// for job sync -- see that file's header comment. There is deliberately
// no separate radio window or background task for this (unlike the
// project it's ported from): every wake syncs jobs AND calendars in the
// same bounded connect-sync-disconnect pass, matching
// docs/architecture.md's deep-sleep model.

#include "config/CalendarConfig.h"

namespace calendar {

// Fetches every feed in `cfg`, computes the soonest upcoming (or currently
// in progress) event across all of them within the lookahead window, and
// saves it via config::CalendarCache -- but only if at least one feed
// fetch succeeded. A wake where every feed is unreachable leaves the
// existing cache untouched, so the idle screen still shows the last known
// next event (see CalendarCache.h) instead of going blank.
//
// No-ops immediately if cfg.count() == 0 (calendars not configured).
void syncCalendars(const config::CalendarConfig& cfg);

}  // namespace calendar
