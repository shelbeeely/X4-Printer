// Host-side test: parses a real XTC file produced by the Pi's independent
// Python encoder (pi-server/focusink_server/xtc_writer.py) with the
// firmware's independent C++ decoder (src/xtc/XtcFormat.h), and checks they
// agree — this is the cross-implementation check that the two from-scratch
// implementations described in docs/architecture.md actually speak the same
// format, not just that each one is internally self-consistent.
//
// fixtures/sample.xtc was generated with:
//   pi-server$ . .venv/bin/activate && python3 -c "... see git history ..."
// Regenerate it any time xtc_writer.py's output shape changes (page count,
// title, etc. must be kept in sync with the assertions below).
//
// No test framework dependency (plain asserts + a process exit code) so
// this builds and runs with nothing but a C++17 compiler and CMake/CTest —
// no network fetch of a test framework required to run `ctest` here.

#include "testutil.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "xtc/XtcFormat.h"

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    std::fprintf(stderr, "failed to open fixture: %s\n", path.c_str());
    std::exit(1);
  }
  std::streamsize size = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(size));
  if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
    std::fprintf(stderr, "failed to read fixture: %s\n", path.c_str());
    std::exit(1);
  }
  return buf;
}

std::string readFixedString(const uint8_t* buf, size_t size) {
  size_t len = 0;
  while (len < size && buf[len] != 0) len++;
  return std::string(reinterpret_cast<const char*>(buf), len);
}

}  // namespace

int main(int argc, char** argv) {
  std::string fixturePath = (argc > 1) ? argv[1] : "fixtures/sample.xtc";
  std::vector<uint8_t> data = readFile(fixturePath);

  CHECK(data.size() >= xtc::kXtcHeaderSize);

  xtc::XtcHeader header;
  bool headerOk = xtc::parseXtcHeader(data.data(), header);
  CHECK(headerOk);
  CHECK(header.mark == xtc::kMagicXtc);
  CHECK(!header.isXtch());
  CHECK(header.version == 0x0100);
  CHECK(header.pageCount == 3);
  CHECK(header.readDirection == 0);
  CHECK(header.hasMetadata == 1);
  CHECK(header.hasThumbnails == 0);
  CHECK(header.hasChapters == 0);
  CHECK(header.metadataOffset == xtc::kXtcHeaderSize);
  CHECK(header.indexOffset == header.metadataOffset + xtc::kMetadataSize);
  CHECK(header.dataOffset == header.indexOffset + header.pageCount * xtc::kIndexEntrySize);

  // Metadata: title/author/publisher fields.
  const uint8_t* meta = data.data() + header.metadataOffset;
  std::string title = readFixedString(meta + 0x00, 128);
  std::string author = readFixedString(meta + 0x80, 64);
  std::string publisher = readFixedString(meta + 0xC0, 32);
  CHECK(title == "Fixture Document");
  CHECK(author == "Test Suite");
  CHECK(publisher == "focusink");  // see docs/xtc-format.md

  uint32_t createTime = xtc::readLE32(meta + 0xF0);
  CHECK(createTime == 1737590000u);

  // Page index + per-page XTG headers.
  for (uint16_t i = 0; i < header.pageCount; i++) {
    const uint8_t* entryBuf = data.data() + header.indexOffset + i * xtc::kIndexEntrySize;
    xtc::XtcPageIndexEntry entry;
    xtc::parseIndexEntry(entryBuf, entry);
    CHECK(entry.width == 800);
    CHECK(entry.height == 480);
    CHECK(entry.offset + entry.size <= data.size());

    const uint8_t* pageBuf = data.data() + entry.offset;
    xtc::XtgHeader xtg;
    bool xtgOk = xtc::parseXtgHeader(pageBuf, xtg);
    CHECK(xtgOk);
    CHECK(xtg.isMonochrome());
    CHECK(!xtg.isGrayscale());
    CHECK(xtg.width == 800);
    CHECK(xtg.height == 480);
    CHECK(xtg.colorMode == 0);
    CHECK(xtg.compression == 0);
    uint32_t expected = xtc::expectedXtgDataSize(800, 480);
    CHECK(expected == 48000u);
    CHECK(xtg.dataSize == expected);
    CHECK(entry.size == xtc::kXtgHeaderSize + xtg.dataSize);
  }

  // A truncated/corrupt buffer must be rejected, not silently parsed —
  // the firmware relies on this to fall back to a bounded per-row copy
  // (see docs/xtc-format.md) instead of reading past the file.
  std::vector<uint8_t> garbage(64, 0xFF);
  xtc::XtcHeader badHeader;
  CHECK(xtc::parseXtcHeader(garbage.data(), badHeader) == false);

  std::printf("XtcFormatTest: all assertions passed (%zu bytes, %u pages)\n", data.size(), header.pageCount);
  return 0;
}
