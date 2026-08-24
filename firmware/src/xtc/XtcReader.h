#pragma once
// SD-backed XTC page renderer. Thin Arduino/FreeInk glue over the
// freestanding xtc::XtcFormat.h parsing — see docs/xtc-format.md
// "Streaming a page into the framebuffer" for the two code paths this
// implements (bulk copy for the common panel-exact-size case, bounded
// per-row copy otherwise) and why neither ever allocates a full-page
// buffer.

#include <SdFat.h>

#include <cstddef>
#include <cstdint>

#include "xtc/XtcFormat.h"

namespace xtc {

enum class RenderResult {
  Ok,
  BadPageIndex,
  IoError,
  UnsupportedFormat,  // e.g. an XTH (grayscale) page — see docs/xtc-format.md
  SizeMismatch,
};

class XtcReader {
 public:
  ~XtcReader() { close(); }

  bool open(const char* path);
  void close();
  bool isOpen() const { return open_; }

  uint16_t pageCount() const { return header_.pageCount; }
  const char* title() const { return title_; }
  const char* author() const { return author_; }

  bool getPageInfo(uint16_t pageIndex, XtcPageIndexEntry& outEntry) const;

  // Renders page `pageIndex` into `framebuffer` (caller-owned, at least
  // panelWidth/8 * panelHeight bytes — pass EInkDisplay::getFrameBuffer()/
  // getBufferSize() directly). Only monochrome (XTG) pages are supported;
  // see docs/xtc-format.md for why XTH is a documented non-goal.
  RenderResult renderPageToFramebuffer(uint16_t pageIndex, uint8_t* framebuffer, size_t framebufferSize,
                                       uint16_t panelWidth, uint16_t panelHeight);

 private:
  mutable FsFile file_;
  bool open_ = false;
  XtcHeader header_;
  char title_[129] = {0};
  char author_[65] = {0};

  bool readAt(uint64_t offset, uint8_t* buf, size_t len) const;
};

}  // namespace xtc
