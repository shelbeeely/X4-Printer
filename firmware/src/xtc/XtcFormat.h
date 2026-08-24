#pragma once
// Freestanding (no Arduino/FreeInk dependency) XTC/XTG byte-layout parsing.
//
// This header is deliberately isolated from any file-I/O so it can be
// compiled and unit tested on the host (see firmware/test/xtc_format/) —
// the same reasoning CrossPoint applies to its own lib/ modules (see
// .skills/hal-and-abstractions in crosspoint-reader): keep format/parsing
// logic freestanding, push device I/O to a thin caller (XtcReader.cpp).
//
// Byte layout is docs/xtc-format.md's subset of the public phrozen/xtx
// spec. All multi-byte fields are little-endian; this file reads them
// field-by-field from a raw byte buffer rather than casting a struct
// pointer over the bytes, so it has no struct-packing/alignment/host-endian
// assumptions at all (safe on ESP32-C3, safe on a host unit test build,
// safe regardless of compiler struct-layout choices).

#include <cstddef>
#include <cstdint>

namespace xtc {

constexpr uint32_t kMagicXtg = 0x00475458;   // "XTG\0"
constexpr uint32_t kMagicXth = 0x00485458;   // "XTH\0"
constexpr uint32_t kMagicXtc = 0x00435458;   // "XTC\0"
constexpr uint32_t kMagicXtch = 0x48435458;  // "XTCH"

constexpr size_t kXtgHeaderSize = 22;
constexpr size_t kXtcHeaderSize = 56;
constexpr size_t kIndexEntrySize = 16;
constexpr size_t kMetadataSize = 256;

struct XtcHeader {
  uint32_t mark = 0;
  uint16_t version = 0;
  uint16_t pageCount = 0;
  uint8_t readDirection = 0;
  uint8_t hasMetadata = 0;
  uint8_t hasThumbnails = 0;
  uint8_t hasChapters = 0;
  uint32_t currentPage = 0;
  uint64_t metadataOffset = 0;
  uint64_t indexOffset = 0;
  uint64_t dataOffset = 0;
  uint64_t thumbOffset = 0;
  uint64_t chapterOffset = 0;

  bool isXtch() const { return mark == kMagicXtch; }
  bool isValidMagic() const { return mark == kMagicXtc || mark == kMagicXtch; }
};

struct XtcPageIndexEntry {
  uint64_t offset = 0;
  uint32_t size = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct XtgHeader {
  uint32_t mark = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t colorMode = 0;
  uint8_t compression = 0;
  uint32_t dataSize = 0;

  bool isMonochrome() const { return mark == kMagicXtg; }
  bool isGrayscale() const { return mark == kMagicXth; }
};

// -- little-endian field readers --------------------------------------------

inline uint16_t readLE16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

inline uint32_t readLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint64_t readLE64(const uint8_t* p) {
  uint64_t lo = readLE32(p);
  uint64_t hi = readLE32(p + 4);
  return lo | (hi << 32);
}

// -- parsers ------------------------------------------------------------

// buf must point at >= kXtcHeaderSize bytes.
inline bool parseXtcHeader(const uint8_t* buf, XtcHeader& out) {
  out.mark = readLE32(buf + 0x00);
  if (out.mark != kMagicXtc && out.mark != kMagicXtch) return false;
  out.version = readLE16(buf + 0x04);
  out.pageCount = readLE16(buf + 0x06);
  out.readDirection = buf[0x08];
  out.hasMetadata = buf[0x09];
  out.hasThumbnails = buf[0x0A];
  out.hasChapters = buf[0x0B];
  out.currentPage = readLE32(buf + 0x0C);
  out.metadataOffset = readLE64(buf + 0x10);
  out.indexOffset = readLE64(buf + 0x18);
  out.dataOffset = readLE64(buf + 0x20);
  out.thumbOffset = readLE64(buf + 0x28);
  out.chapterOffset = readLE64(buf + 0x30);
  return true;
}

// buf must point at >= kIndexEntrySize bytes.
inline void parseIndexEntry(const uint8_t* buf, XtcPageIndexEntry& out) {
  out.offset = readLE64(buf + 0x00);
  out.size = readLE32(buf + 0x08);
  out.width = readLE16(buf + 0x0C);
  out.height = readLE16(buf + 0x0E);
}

// buf must point at >= kXtgHeaderSize bytes.
inline bool parseXtgHeader(const uint8_t* buf, XtgHeader& out) {
  out.mark = readLE32(buf + 0x00);
  if (out.mark != kMagicXtg && out.mark != kMagicXth) return false;
  out.width = readLE16(buf + 0x04);
  out.height = readLE16(buf + 0x06);
  out.colorMode = buf[0x08];
  out.compression = buf[0x09];
  out.dataSize = readLE32(buf + 0x0A);
  return true;
}

// Expected XTG data size for a width x height monochrome (1bpp) bitmap,
// matching docs/xtc-format.md / the Pi encoder's row-major MSB-first
// packing: ((width + 7) / 8) * height.
inline uint32_t expectedXtgDataSize(uint16_t width, uint16_t height) {
  return uint32_t((width + 7) / 8) * uint32_t(height);
}

}  // namespace xtc
