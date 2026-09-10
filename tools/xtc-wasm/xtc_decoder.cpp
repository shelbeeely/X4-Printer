// WASM build of an XTC page decoder for the X4's on-device web UI
// (firmware/src/ui/WebUiServer.cpp's job-list "Preview" button) — lets a
// phone browser decode and show a print job before approving it, entirely
// client-side (no server-side rendering, no internet access needed once
// the .wasm/.js bytes and the job's raw XTC file have been fetched from
// the X4 itself).
//
// Reuses firmware/src/xtc/XtcFormat.h verbatim for the byte-layout parsing
// (already freestanding/host-portable — this session's own
// firmware/test/xtc_format/ suite already exercises it) rather than
// reimplementing the XTC/XTG spec a second time. What's new here is only
// the pixel-unpacking target: firmware/src/xtc/XtcReader.cpp streams the
// same packed 1bpp rows straight into an e-ink framebuffer; this decodes
// them into a full 1-byte-per-pixel grayscale buffer instead, since that's
// what a browser <canvas>'s ImageData needs. XTH (grayscale) pages are
// rejected here for the same reason XtcReader.cpp rejects them — see
// docs/xtc-format.md.
//
// Bit polarity (confirmed against pi-server/focusink_server/
// xtc_writer.py's encode_xtg_page() docstring and
// firmware/src/xtc/XtcReader.cpp's "0xFF = all-white" comment): bit=1 is a
// white pixel, bit=0 is black.

#include "../../firmware/src/xtc/XtcFormat.h"

#include <cstdlib>
#include <cstdint>

extern "C" {

// Decodes page `pageIndex` of an in-memory XTC file (the caller writes
// `xtcBytes`/`len` into this module's linear memory, e.g. via
// `Module.HEAPU8.set(...)` after a `fetch()`) into a freshly `malloc`'d
// grayscale buffer, one byte per pixel (0 = black, 255 = white), row-major
// top-to-bottom. Returns nullptr (leaving *outWidth/*outHeight untouched)
// on any parse/format/bounds error, mirroring XtcReader's own rejections
// — a caller should treat null as "couldn't preview this page", not crash.
// Caller must release the result with xtc_free().
uint8_t* xtc_decode_page(const uint8_t* xtcBytes, uint32_t len, uint16_t pageIndex, uint16_t* outWidth,
                          uint16_t* outHeight) {
  if (xtcBytes == nullptr || len < xtc::kXtcHeaderSize) return nullptr;

  xtc::XtcHeader header;
  if (!xtc::parseXtcHeader(xtcBytes, header)) return nullptr;
  if (pageIndex >= header.pageCount) return nullptr;

  uint64_t entryOffset = header.indexOffset + uint64_t(pageIndex) * xtc::kIndexEntrySize;
  if (entryOffset + xtc::kIndexEntrySize > len) return nullptr;

  xtc::XtcPageIndexEntry entry;
  xtc::parseIndexEntry(xtcBytes + entryOffset, entry);
  if (entry.offset + xtc::kXtgHeaderSize > len) return nullptr;

  xtc::XtgHeader xtg;
  if (!xtc::parseXtgHeader(xtcBytes + entry.offset, xtg)) return nullptr;
  if (!xtg.isMonochrome()) return nullptr;  // XTH unsupported — see docs/xtc-format.md

  uint32_t expected = xtc::expectedXtgDataSize(xtg.width, xtg.height);
  if (xtg.dataSize != expected) return nullptr;

  uint64_t dataOffset = entry.offset + xtc::kXtgHeaderSize;
  if (dataOffset + xtg.dataSize > len) return nullptr;
  if (xtg.width == 0 || xtg.height == 0) return nullptr;

  const uint8_t* packed = xtcBytes + dataOffset;
  uint32_t rowBytes = uint32_t((xtg.width + 7) / 8);

  uint8_t* pixels = static_cast<uint8_t*>(std::malloc(size_t(xtg.width) * size_t(xtg.height)));
  if (pixels == nullptr) return nullptr;

  for (uint16_t y = 0; y < xtg.height; y++) {
    const uint8_t* srcRow = packed + size_t(y) * rowBytes;
    uint8_t* dstRow = pixels + size_t(y) * xtg.width;
    for (uint16_t x = 0; x < xtg.width; x++) {
      uint8_t byte = srcRow[x / 8];
      uint8_t bit = (byte >> (7 - (x % 8))) & 1;
      dstRow[x] = bit ? 255 : 0;
    }
  }

  *outWidth = xtg.width;
  *outHeight = xtg.height;
  return pixels;
}

// Returns the page count of an in-memory XTC file, or 0 on a parse error
// — lets the preview UI show page-navigation controls without a second
// round trip.
uint16_t xtc_page_count(const uint8_t* xtcBytes, uint32_t len) {
  if (xtcBytes == nullptr || len < xtc::kXtcHeaderSize) return 0;
  xtc::XtcHeader header;
  if (!xtc::parseXtcHeader(xtcBytes, header)) return 0;
  return header.pageCount;
}

// Releases a buffer returned by xtc_decode_page().
void xtc_free(uint8_t* ptr) { std::free(ptr); }

}  // extern "C"
