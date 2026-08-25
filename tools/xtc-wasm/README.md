# XTC WASM decoder (X4 web UI document preview)

Compiles `xtc_decoder.cpp` — a client-side decoder for the X4's XTC print
format — to WebAssembly, so a phone browser can decode and preview a job
before approving it, entirely offline (no internet access needed, matching
the on-device web UI's hotspot-mode constraint — see `docs/security.md`
"On-device Web UI").

Reuses `firmware/src/xtc/XtcFormat.h` verbatim for the byte-layout parsing
(the same header `firmware/test/xtc_format/` already host-tests) — this
directory only adds the pixel-unpacking + WASM export surface, not a
second implementation of the XTC/XTG spec.

## Build

```sh
sudo apt-get install -y emscripten   # or use emsdk if you prefer
bash build.sh
python3 ../../firmware/src/ui/wasm/generate_header.py
```

`build.sh` writes `xtc_decoder.wasm`/`xtc_decoder.js` into
`firmware/src/ui/wasm/` (committed — small, ~9KB + ~18KB). The header
generator turns those into `firmware/src/ui/XtcDecoderWasmData.h`
(committed, generated — `WebUiServer.cpp` `#include`s it directly, no
filesystem access needed on the device). Commit all three regenerated
files together whenever `xtc_decoder.cpp` changes.

CI (`.github/workflows/tests.yml`) rebuilds from source on every push and
fails if either the `.wasm`/`.js` or the generated header no longer
matches what's committed here — a "did you forget to regenerate" check,
not a publish step (unlike the ESP32 firmware's own best-effort CI build,
emscripten installs reliably in CI, so this one is a required check).

## Exported functions

- `uint8_t* xtc_decode_page(const uint8_t* xtcBytes, uint32_t len, uint16_t pageIndex, uint16_t* outWidth, uint16_t* outHeight)`
  — decodes one page to a `malloc`'d grayscale buffer (1 byte/pixel, 0=black/255=white,
  row-major). Returns `nullptr` on any parse/format/bounds error (including
  the non-monochrome XTH pages this project doesn't generate — see
  `docs/xtc-format.md`).
- `uint16_t xtc_page_count(const uint8_t* xtcBytes, uint32_t len)` —
  returns 0 on a parse error.
- `void xtc_free(uint8_t* ptr)` — releases a buffer from `xtc_decode_page`.

## Verified how

There's no unit test committed for this (it's a thin export shim over
already-tested parsing logic), but it was manually verified end-to-end in
a real headless browser (Playwright + the pre-installed Chromium) against
`firmware/test/xtc_format/fixtures/sample.xtc` — the same fixture
`firmware/test/xtc_format/XtcFormatTest.cpp` cross-checks against the Pi's
independent Python encoder. Confirmed: page count 3, each page decodes to
800×480 with every pixel cleanly 0 or 255 (no stray bits from an off-by-one
in the unpacking loop), and an out-of-range page index is rejected
(returns `nullptr`) rather than reading out of bounds.
