# XTC/XTG File Format (as used by this project)

The X4 stores print-inbox documents on its SD card as native **XTC**
containers — the same format CrossPoint Reader uses for comics/manga. Full
byte-level spec:
[phrozen/xtx SPEC.md](https://github.com/phrozen/xtx/blob/main/SPEC.md)
(vendored knowledge only — this repo does not depend on the Go module; the
Pi-side encoder is a from-scratch pure-Python implementation of the same
spec, and the firmware-side reader is a from-scratch C++ implementation).

## What this project produces

`pi-server/xteink_print_server/xtc_writer.py` always emits **XTC** (not
XTCH): every page is a monochrome **XTG** bitmap (1 bit/pixel, row-major,
MSB-first), pre-dithered and pre-scaled to the X4's panel resolution
(800x480, `EInkDisplay::DISPLAY_WIDTH/HEIGHT`).

This is a deliberate simplification over the full spec:

- **XTH (2-bit, 4-level grayscale) pages are not generated.** XTH's pixel
  data is packed in *column-major* order with a non-linear level mapping
  (see the spec's "Data Storage Details"), which only matters for photo-like
  images. Print-inbox documents (PDFs, receipts, letters, invoices) already
  dither well to 1-bit monochrome at print-inbox scale, and skipping XTH
  keeps both the converter and the firmware reader an order of magnitude
  simpler: an XTG page is streamed straight from the SD file into the
  display's framebuffer memory (§ below), with no bit-plane
  transpose/repack step and no extra RAM.
- **Thumbnails are not generated.** The Print Inbox UI shows text-based list
  rows (title, page count, size), not cover art, so `hasThumbnails` is
  always `0` and the thumbnail area is omitted.
- **Chapters are not generated.** Print jobs are flat page sequences.
- **Metadata is always written** (`hasMetadata = 1`): `title` is the job's
  display name (from the print job's `document-name`/`job-name` IPP
  attribute, or the source filename), `createTime` is the job's creation
  time, `publisher` is fixed to `"xteink-print-inbox"` so the reader can
  distinguish print-inbox documents from books if the SD card is ever
  shared with CrossPoint Reader.

Firmware's `firmware/src/xtc/XtcReader.*` still *parses* `hasThumbnails` /
`hasChapters` / the XTH bit-depth field correctly (reads `getBitDepth()` from
the page index and takes the XTG fast path only when it is `1`), so a hand
authored or future XTCH file dropped onto the SD card by another tool
degrades gracefully — page rendering for XTH falls back to a documented
"unsupported page format" placeholder screen rather than corrupting the
display, but is not a rendering path this project exercises by default.

## Why XTC (not raw PDF) is what ships to the device

1. **No PDF parser on an ESP32-C3.** Rendering a PDF page requires a font
   engine, a PDF object graph, and typically megabytes of working memory —
   none of which fit in the ~400KB of usable RAM left after Wi-Fi/TLS on a
   C3. Rasterizing on the Pi (which has a real CPU and PyMuPDF) and shipping
   a pre-rendered, panel-native bitmap container is the only approach that
   keeps the firmware within `docs/architecture.md`'s memory budget.
2. **Streamable.** Each page is a fixed-size, independently addressable
   region (`pageIndex[i].offset`/`size`), so the reader seeks directly to a
   page and reads it in bounded chunks without touching the rest of the
   file — no full-document parse, no full-document RAM buffer, and paging
   through a 40-page document costs the same ~2KB of stack-resident read
   buffer as a 2-page one.
3. **One format, two roles.** Because it's the same container CrossPoint
   already reads, a user who also runs CrossPoint on their X4 can open a
   downloaded print-inbox document from CrossPoint's own library browser
   without any conversion step.

## Streaming a page into the framebuffer

Because every generated page is exactly `EInkDisplay::DISPLAY_WIDTH` x
`DISPLAY_HEIGHT` (padding/letterboxing is done on the Pi at conversion time,
never on-device), `dataSize` for every page equals
`FreeInkDisplay::BUFFER_SIZE` (`DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT` =
48000 bytes for the X4's 800x480 panel) and the XTG row-major/MSB-first
pixel layout is byte-for-byte what `FreeInkDisplay::setFramebuffer()`/
`drawImage()` expect for a full-screen image. `XtcReader::renderPage()`
therefore:

1. Seeks to `pageIndex[i].offset + 22` (skip the embedded XTG header — the
   reader already knows width/height/dataSize from the index entry).
2. Reads the file in `XTC_STREAM_CHUNK_BYTES` (2048 B) chunks directly into
   the caller-supplied framebuffer pointer (`display.getFrameBuffer()`),
   advancing the destination offset — never allocating a second
   whole-page buffer.
3. Verifies the byte count read equals the index entry's `size`; on a short
   read (corrupt/truncated file) it leaves the framebuffer untouched beyond
   the partial write and reports an error so the UI can show a "page
   unreadable" state instead of a torn frame.

If a page's `width`/`height` in the index do not match the panel's active
resolution (e.g. a file authored for X3's 792x528), the reader falls back to
a bounded per-row copy into a small stack buffer that letterboxes/crops
instead of a raw memcpy — still no full-page heap allocation.
