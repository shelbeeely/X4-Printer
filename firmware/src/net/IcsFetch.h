#pragma once
// Streams an ICS calendar feed over HTTPS straight into an IcsParser --
// same streaming-chunk discipline as SyncClient::downloadJobToSd (never
// buffers the whole feed in RAM), same WiFiClientSecure/HTTPClient stack
// SyncClient.cpp already uses, but a different TLS trust model: calendar
// feeds are on the open internet (Google Calendar, etc.), not the user's
// own Pi, so there's no CA to pin the way /system/pi_ca.pem does for the
// sync API. This always calls WiFiClientSecure::setInsecure() -- the same
// fallback SyncClient.cpp's configureClientForEndpoint() already uses for
// the relay when no cert is pinned (see that file's comment); accepted
// here unconditionally since there is no arbitrary-third-party-host
// equivalent of cert pinning to fall back FROM.

#include <cstddef>

#include <WString.h>

namespace calendar {
class IcsParser;
}

namespace net {

enum class IcsFetchResult {
  Ok,
  ConnectFailed,
  HttpError,   // non-2xx status
  ParseAborted,
};

// Streams `url`'s response body into `parser` (calling parser.feed() per
// chunk, then parser.finish() once the body is fully read). Bounded by
// kHttpTimeoutMs (SyncClient.h) the same way every other network call in
// this firmware is.
IcsFetchResult fetchIcs(const String& url, calendar::IcsParser& parser);

}  // namespace net
