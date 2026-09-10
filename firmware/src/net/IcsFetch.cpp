#include "net/IcsFetch.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "calendar/IcsParser.h"
#include "net/SyncClient.h"  // kStreamChunkBytes / kHttpTimeoutMs

namespace net {

IcsFetchResult fetchIcs(const String& url, calendar::IcsParser& parser) {
  WiFiClientSecure client;
  client.setInsecure();  // see header comment: no pinned CA for arbitrary calendar hosts

  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);

  if (!http.begin(client, url)) return IcsFetchResult::ConnectFailed;

  int code = http.GET();
  if (code < 200 || code >= 300) {
    http.end();
    return IcsFetchResult::HttpError;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[kStreamChunkBytes];
  uint32_t lastByteAt = millis();

  while (http.connected()) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (!stream->connected()) break;
      if (millis() - lastByteAt > kHttpTimeoutMs) break;  // stalled connection
      delay(10);
      continue;
    }
    size_t toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
    int n = stream->readBytes(buf, toRead);
    if (n <= 0) break;
    parser.feed(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    lastByteAt = millis();
  }
  http.end();

  parser.finish();
  return IcsFetchResult::Ok;
}

}  // namespace net
