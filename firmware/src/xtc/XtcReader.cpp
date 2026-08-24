#include "xtc/XtcReader.h"

#include <SDCardManager.h>

#include <cstring>

namespace xtc {

namespace {
constexpr size_t kBulkChunkBytes = 2048;
constexpr size_t kMaxRowBytes = 128;  // covers panels up to 1024px wide; X4 is 800px (100 bytes/row)

std::string readFixedStringHelper(const uint8_t* buf, size_t maxLen) {
  size_t len = 0;
  while (len < maxLen && buf[len] != 0) len++;
  return std::string(reinterpret_cast<const char*>(buf), len);
}
}  // namespace

bool XtcReader::readAt(uint64_t offset, uint8_t* buf, size_t len) const {
  if (!file_.seekSet(offset)) return false;
  int n = file_.read(buf, len);
  return n >= 0 && static_cast<size_t>(n) == len;
}

bool XtcReader::open(const char* path) {
  close();

  file_ = SdMan.open(path, O_RDONLY);
  if (!file_) return false;

  uint8_t headerBuf[kXtcHeaderSize];
  if (!readAt(0, headerBuf, kXtcHeaderSize) || !parseXtcHeader(headerBuf, header_)) {
    file_.close();
    return false;
  }

  if (header_.hasMetadata) {
    uint8_t metaBuf[kMetadataSize];
    if (readAt(header_.metadataOffset, metaBuf, kMetadataSize)) {
      auto t = readFixedStringHelper(metaBuf + 0x00, 128);
      auto a = readFixedStringHelper(metaBuf + 0x80, 64);
      std::strncpy(title_, t.c_str(), sizeof(title_) - 1);
      std::strncpy(author_, a.c_str(), sizeof(author_) - 1);
    }
  }

  open_ = true;
  return true;
}

void XtcReader::close() {
  if (open_) {
    file_.close();
    open_ = false;
  }
  header_ = XtcHeader{};
  title_[0] = '\0';
  author_[0] = '\0';
}

bool XtcReader::getPageInfo(uint16_t pageIndex, XtcPageIndexEntry& outEntry) const {
  if (!open_ || pageIndex >= header_.pageCount) return false;
  uint8_t buf[kIndexEntrySize];
  uint64_t offset = header_.indexOffset + uint64_t(pageIndex) * kIndexEntrySize;
  if (!readAt(offset, buf, kIndexEntrySize)) return false;
  parseIndexEntry(buf, outEntry);
  return true;
}

RenderResult XtcReader::renderPageToFramebuffer(uint16_t pageIndex, uint8_t* framebuffer, size_t framebufferSize,
                                                uint16_t panelWidth, uint16_t panelHeight) {
  if (!open_) return RenderResult::IoError;

  XtcPageIndexEntry entry;
  if (!getPageInfo(pageIndex, entry)) return RenderResult::BadPageIndex;

  uint8_t xtgHeaderBuf[kXtgHeaderSize];
  if (!readAt(entry.offset, xtgHeaderBuf, kXtgHeaderSize)) return RenderResult::IoError;

  XtgHeader xtg;
  if (!parseXtgHeader(xtgHeaderBuf, xtg)) return RenderResult::IoError;
  if (!xtg.isMonochrome()) {
    // XTH (grayscale) pages are a documented non-goal for this reader —
    // see docs/xtc-format.md. The UI shows a "page format unsupported"
    // placeholder rather than attempting to interpret column-major
    // dual-plane data as if it were row-major 1bpp.
    return RenderResult::UnsupportedFormat;
  }

  uint32_t expected = expectedXtgDataSize(xtg.width, xtg.height);
  if (xtg.dataSize != expected) return RenderResult::SizeMismatch;

  uint64_t dataOffset = entry.offset + kXtgHeaderSize;
  uint32_t panelRowBytes = uint32_t((panelWidth + 7) / 8);
  size_t requiredFramebufferBytes = size_t(panelRowBytes) * panelHeight;
  if (framebufferSize < requiredFramebufferBytes) return RenderResult::SizeMismatch;

  if (xtg.width == panelWidth && xtg.height == panelHeight) {
    // Fast path: the page is exactly panel-sized (the common case — the Pi
    // encoder always renders at the panel's resolution, see
    // docs/xtc-format.md). Stream straight into the framebuffer in bulk
    // chunks; no per-row seeks, no extra buffer beyond the chunk itself.
    uint32_t remaining = xtg.dataSize;
    uint8_t* dst = framebuffer;
    uint64_t readOffset = dataOffset;
    while (remaining > 0) {
      size_t chunk = remaining < kBulkChunkBytes ? remaining : kBulkChunkBytes;
      if (!readAt(readOffset, dst, chunk)) return RenderResult::IoError;
      dst += chunk;
      readOffset += chunk;
      remaining -= chunk;
    }
    return RenderResult::Ok;
  }

  // Fallback: letterbox/crop a differently-sized page (e.g. authored for
  // another panel) into the active panel geometry, one source row at a
  // time — bounded to kMaxRowBytes regardless of page width, never a
  // full-page buffer.
  uint32_t srcRowBytes = uint32_t((xtg.width + 7) / 8);
  if (srcRowBytes > kMaxRowBytes) return RenderResult::UnsupportedFormat;

  // White-fill the destination first (0xFF = all-white per docs/xtc-format.md).
  std::memset(framebuffer, 0xFF, requiredFramebufferBytes);

  int32_t yOffset = (int32_t(panelHeight) - int32_t(xtg.height)) / 2;
  int32_t xOffsetPixels = (int32_t(panelWidth) - int32_t(xtg.width)) / 2;
  uint8_t rowBuf[kMaxRowBytes];

  for (uint16_t srcY = 0; srcY < xtg.height; srcY++) {
    int32_t dstY = yOffset + srcY;
    if (dstY < 0 || dstY >= panelHeight) continue;
    if (!readAt(dataOffset + uint64_t(srcY) * srcRowBytes, rowBuf, srcRowBytes)) return RenderResult::IoError;

    uint8_t* dstRow = framebuffer + size_t(dstY) * panelRowBytes;
    if (xOffsetPixels == 0) {
      std::memcpy(dstRow, rowBuf, srcRowBytes < panelRowBytes ? srcRowBytes : panelRowBytes);
    } else {
      // Bit-level shift for non-byte-aligned centering is out of scope for
      // this prototype's fallback path (it only needs to handle
      // occasional cross-panel content, not be pixel-perfect); shift by
      // whole bytes only, which is exact whenever the width difference is
      // a multiple of 8 and a bounded, documented approximation otherwise.
      int32_t byteOffset = xOffsetPixels / 8;
      for (uint32_t srcByte = 0; srcByte < srcRowBytes; srcByte++) {
        int32_t dstByte = int32_t(srcByte) + byteOffset;
        if (dstByte < 0 || dstByte >= int32_t(panelRowBytes)) continue;
        dstRow[dstByte] = rowBuf[srcByte];
      }
    }
  }
  return RenderResult::Ok;
}

}  // namespace xtc
