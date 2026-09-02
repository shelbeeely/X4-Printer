# XTC WASM decoder/encoder (X4 web UI document preview + direct upload)

Two independent WASM modules compiled from this directory, sharing one
build/embed pipeline:

- `xtc_decoder.cpp` — decodes the X4's XTC print format client-side, so a
  phone browser can preview a job before approving it, entirely offline
  (no internet access needed, matching the on-device web UI's
  hotspot-mode constraint — see `docs/security.md` "On-device Web UI").
- `xtc_encoder.cpp` — the reverse direction: turns a phone-picked
  photo/screenshot into a real single-page XTC file entirely client-side,
  behind the job-list page's "Upload" button (see
  `docs/architecture.md`'s "Direct upload" section). A third independent
  implementation of the XTC *write* side, alongside `xtc_writer.py` (Pi)
  and this firmware's own reader — checked against the same
  `firmware/src/xtc/XtcFormat.h` constants all three already agree on.

Both reuse `firmware/src/xtc/XtcFormat.h` verbatim for the byte-layout
(the same header `firmware/test/xtc_format/` already host-tests) — this
directory only adds the pixel (un)packing + WASM export surface, never a
second implementation of the XTC/XTG spec itself.

## Build

```sh
sudo apt-get install -y emscripten   # or use emsdk if you prefer

# Decoder
bash build.sh
python3 ../../firmware/src/ui/wasm/generate_header.py

# Encoder
bash build_encoder.sh
python3 ../../firmware/src/ui/wasm/generate_encoder_header.py
```

`build.sh`/`build_encoder.sh` write `xtc_decoder.{wasm,js}` /
`xtc_encoder.{wasm,js}` into `firmware/src/ui/wasm/` (committed — small,
~9KB+~18KB and ~10KB+~18KB respectively). Each header generator
gzip-compresses its pair into `firmware/src/ui/XtcDecoderWasmData.h` /
`XtcEncoderWasmData.h` (committed, generated — `WebUiServer.cpp`
`#include`s both directly and serves all four with a
`Content-Encoding: gzip` header via its `sendGzip()` helper, no
filesystem access or on-device compression needed). Commit the rebuilt
`.wasm`/`.js` and the regenerated header together whenever the matching
`.cpp` changes.

CI (`.github/workflows/tests.yml`) rebuilds both modules from source on
every push and fails if either's `.wasm`/`.js` or generated header no
longer matches what's committed here — a "did you forget to regenerate"
check, not a publish step (unlike the ESP32 firmware's own best-effort CI
build, emscripten installs reliably in CI, so this one is a required
check).

## Exported functions

### Decoder (`xtc_decoder.cpp`)

- `uint8_t* xtc_decode_page(const uint8_t* xtcBytes, uint32_t len, uint16_t pageIndex, uint16_t* outWidth, uint16_t* outHeight)`
  — decodes one page to a `malloc`'d grayscale buffer (1 byte/pixel, 0=black/255=white,
  row-major). Returns `nullptr` on any parse/format/bounds error (including
  the non-monochrome XTH pages this project doesn't generate — see
  `docs/xtc-format.md`).
- `uint16_t xtc_page_count(const uint8_t* xtcBytes, uint32_t len)` —
  returns 0 on a parse error.
- `void xtc_free(uint8_t* ptr)` — releases a buffer from `xtc_decode_page`.

### Encoder (`xtc_encoder.cpp`)

- `uint8_t* xtc_encode_single_page(const uint8_t* grayscalePixels, uint16_t width, uint16_t height, const char* title, uint32_t createTime, uint32_t* outLen)`
  — Floyd-Steinberg dithers an already-letterboxed grayscale buffer (same
  shape `xtc_decode_page` produces, so the two are symmetric) to 1bpp and
  wraps a complete one-page XTC container. Returns `nullptr` only on
  invalid input (null buffer or zero width/height); caller reads
  `*outLen` bytes then releases with `xtc_free()`.
- `void xtc_free(uint8_t* ptr)` — same free wrapper, shared name/shape
  with the decoder's (each is its own WASM module, so there's no symbol
  collision — see `xtc_encoder.js`'s `EXPORT_NAME=createXtcEncoderModule`
  vs. the decoder's own module name).

## Verified how

Both were manually verified end-to-end, not by a committed unit test
(they're thin export shims over already-tested/reused parsing logic):

- **Decoder**: a real headless browser (Playwright + the pre-installed
  Chromium) against `firmware/test/xtc_format/fixtures/sample.xtc` — the
  same fixture `firmware/test/xtc_format/XtcFormatTest.cpp` cross-checks
  against the Pi's independent Python encoder. Confirmed: page count 3,
  each page decodes to 800×480 with every pixel cleanly 0 or 255 (no
  stray bits from an off-by-one in the unpacking loop), and an
  out-of-range page index is rejected (returns `nullptr`) rather than
  reading out of bounds.
- **Encoder**: a Node.js cross-implementation round-trip — encode a
  dithering-neutral (pure black/white checkerboard) test page with
  `xtc_encoder.js`, decode the exact same bytes with the *existing*
  `xtc_decoder.js`, and confirm they agree: every pixel matches exactly,
  page count is 1, dimensions match, and the header magic/version/title/
  publisher fields all decode correctly — the same "check one
  implementation against another" approach `XtcFormatTest.cpp` already
  uses for the Pi/firmware pair, now covering the encoder too.
