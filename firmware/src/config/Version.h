#pragma once
// Firmware version string, shown on the Settings screen's Device Info tab
// (ui/InboxUI.cpp) and available for any future diagnostics surface (e.g.
// ui/WebUiServer.cpp's status endpoint). Bump manually on release; this
// project has no build-time version-stamping step to keep in sync with.

namespace config {

constexpr const char* kFirmwareVersion = "0.1.0";

}  // namespace config
