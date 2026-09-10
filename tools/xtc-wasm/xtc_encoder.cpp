// WASM build of an XTC page encoder for the X4's on-device web UI
// (firmware/src/ui/WebUiServer.cpp's job-list "Upload" button) — lets a
// phone browser turn a picked photo/screenshot into a real XTC file
// entirely client-side, so it can be uploaded straight to the X4 and read
// with no Pi involved. The browser's own <canvas> already does the
// letterbox-resize and grayscale extraction (no image codec belongs in
// this module); what's here is only the XTC-specific part: Floyd-
// Steinberg dithering to 1bpp and packing a complete single-page XTC
// container, byte-for-byte matching what pi-server/focusink_server/
// xtc_writer.py's encode_xtc()/encode_xtg_page() produce (re-derived from
// that module's exact byte offsets, not copied — this is a third
// independent implementation of the write side, checked against the same
// firmware/src/xtc/XtcFormat.h constants firmware's own reader and the
// existing xtc_decoder.cpp already agree on).
//
// Companion to xtc_decoder.cpp in this same directory — decode direction
// there, encode direction here, same build/embed pipeline (see README.md).

#include "../../firmware/src/xtc/XtcFormat.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {

namespace {

void writeLE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
}

void writeLE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

void writeLE64(uint8_t* p, uint64_t v) {
  writeLE32(p, uint32_t(v));
  writeLE32(p + 4, uint32_t(v >> 32));
}

// Zero-fills `size` bytes at `dst` then copies as much of `value` as fits
// (truncated, always NUL-terminated within the field) — mirrors
// xtc_writer.py's _pack_fixed_string() exactly.
void packFixedString(uint8_t* dst, size_t size, const char* value) {
  std::memset(dst, 0, size);
  if (value == nullptr) return;
  size_t len = std::strlen(value);
  if (len > size - 1) len = size - 1;
  std::memcpy(dst, value, len);
}

}  // namespace

// Encodes a single already-letterboxed grayscale page (one byte/pixel,
// 0=black/255=white, row-major top-to-bottom — the same shape
// xtc_decode_page() produces, so the two are symmetric) into a complete,
// ready-to-upload one-page XTC file: Floyd-Steinberg dithers to 1bpp,
// packs it XTG-style, and wraps a full XTC container (header + metadata
// with `title`/`createTime`/publisher fixed to "focusink" + one index
// entry + the XTG page). Returns nullptr on invalid input (null buffer or
// zero width/height); never fails otherwise. Caller reads *outLen bytes
// then releases the result with xtc_free().
uint8_t* xtc_encode_single_page(const uint8_t* grayscalePixels, uint16_t width, uint16_t height, const char* title,
                                 uint32_t createTime, uint32_t* outLen) {
  if (grayscalePixels == nullptr || width == 0 || height == 0 || outLen == nullptr) return nullptr;

  const uint32_t rowBytes = (uint32_t(width) + 7) / 8;
  const uint32_t xtgDataSize = rowBytes * uint32_t(height);
  const uint32_t xtgTotalSize = uint32_t(xtc::kXtgHeaderSize) + xtgDataSize;

  const uint64_t metadataOffset = xtc::kXtcHeaderSize;
  const uint64_t indexOffset = metadataOffset + xtc::kMetadataSize;
  const uint64_t dataOffset = indexOffset + xtc::kIndexEntrySize;  // exactly one page
  const uint64_t totalSize = dataOffset + xtgTotalSize;

  uint8_t* buf = static_cast<uint8_t*>(std::malloc(size_t(totalSize)));
  if (buf == nullptr) return nullptr;
  std::memset(buf, 0, size_t(totalSize));

  // -- XTC header (56 bytes) --
  uint8_t* h = buf;
  writeLE32(h + 0x00, xtc::kMagicXtc);
  writeLE16(h + 0x04, 0x0100);  // version 1.0
  writeLE16(h + 0x06, 1);       // pageCount
  h[0x08] = 0;                  // readDirection: left-to-right
  h[0x09] = 1;                  // hasMetadata
  h[0x0A] = 0;                  // hasThumbnails
  h[0x0B] = 0;                  // hasChapters
  writeLE32(h + 0x0C, 0);       // currentPage
  writeLE64(h + 0x10, metadataOffset);
  writeLE64(h + 0x18, indexOffset);
  writeLE64(h + 0x20, dataOffset);
  writeLE64(h + 0x28, 0);  // thumbOffset (unused)
  writeLE64(h + 0x30, 0);  // chapterOffset (unused)

  // -- metadata (256 bytes) — field offsets match xtc_writer.py exactly --
  uint8_t* m = buf + metadataOffset;
  packFixedString(m + 0x00, 128, title);       // title
  packFixedString(m + 0x80, 64, "");           // author (unused here)
  packFixedString(m + 0xC0, 32, "focusink");   // publisher tag
  packFixedString(m + 0xE0, 16, "en-US");      // language
  writeLE32(m + 0xF0, createTime);
  writeLE16(m + 0xF4, 0xFFFF);
  writeLE16(m + 0xF6, 0);
  // 0xF8-0xFF reserved, already zeroed by the buffer memset above

  // -- index (16 bytes, one entry) --
  uint8_t* idx = buf + indexOffset;
  writeLE64(idx + 0x00, dataOffset);
  writeLE32(idx + 0x08, xtgTotalSize);
  writeLE16(idx + 0x0C, width);
  writeLE16(idx + 0x0E, height);

  // -- XTG page: header (22 bytes) + packed 1bpp data --
  uint8_t* xtg = buf + dataOffset;
  writeLE32(xtg + 0x00, xtc::kMagicXtg);
  writeLE16(xtg + 0x04, width);
  writeLE16(xtg + 0x06, height);
  xtg[0x08] = 0;  // colorMode: monochrome
  xtg[0x09] = 0;  // compression: none
  writeLE32(xtg + 0x0A, xtgDataSize);
  // 0x0E-0x15 (unused md5 field) stay zero

  uint8_t* packed = xtg + xtc::kXtgHeaderSize;

  // Floyd-Steinberg error diffusion (right 7/16, bottom-left 3/16,
  // bottom 5/16, bottom-right 1/16) — matches prepare_page_image()'s
  // documented dithering choice ("much better print-document legibility
  // than a hard threshold"). Operates on a float copy so the caller's own
  // grayscalePixels buffer is never mutated.
  std::vector<float> row(width);
  std::vector<float> carryErr(width, 0.0f);
  std::vector<float> nextErr(width, 0.0f);

  for (uint16_t y = 0; y < height; y++) {
    const uint8_t* srcRow = grayscalePixels + size_t(y) * width;
    for (uint16_t x = 0; x < width; x++) {
      row[x] = float(srcRow[x]) + carryErr[x];
    }
    std::fill(nextErr.begin(), nextErr.end(), 0.0f);

    uint8_t* dstRow = packed + size_t(y) * rowBytes;
    for (uint16_t x = 0; x < width; x++) {
      float src = row[x];
      bool white = src >= 128.0f;
      if (white) dstRow[x / 8] |= uint8_t(0x80 >> (x % 8));  // bit=1 white -- matches encode_xtg_page's layout
      float err = src - (white ? 255.0f : 0.0f);

      if (x + 1 < width) row[x + 1] += err * (7.0f / 16.0f);
      if (y + 1 < height) {
        if (x > 0) nextErr[x - 1] += err * (3.0f / 16.0f);
        nextErr[x] += err * (5.0f / 16.0f);
        if (x + 1 < width) nextErr[x + 1] += err * (1.0f / 16.0f);
      }
    }
    carryErr.swap(nextErr);
  }

  *outLen = uint32_t(totalSize);
  return buf;
}

// Releases a buffer returned by xtc_encode_single_page().
void xtc_free(uint8_t* ptr) { std::free(ptr); }

}  // extern "C"
