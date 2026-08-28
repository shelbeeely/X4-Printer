# X4 on-device web UI pages

`login.html` and `joblist.html` are the human-edited source for the X4's
two on-device web UI pages (served by `WebUiServer.cpp`'s `handleRoot()`).
Edit these directly — they're plain, readable HTML/CSS/JS, not generated.

## Regenerating the embedded header

```sh
python3 generate_pages_header.py
```

Gzip-compresses both pages and writes `../PagesData.h` (committed,
generated — `WebUiServer.cpp` `#include`s it directly and serves both
with a `Content-Encoding: gzip` header via its `sendGzip()` helper, so
there's no on-device compression work and no filesystem access needed).
Real measurement on the current pages: `login.html` 2820 → 1415 bytes
(50%), `joblist.html` 7435 → 3044 bytes (41%) — meaningful savings both
on firmware flash usage and on the bytes actually sent over the X4's own
constrained hotspot AP (see `docs/architecture.md`'s "On-device Web UI"
section). Every browser capable of running this UI's client-side WASM
decode (`tools/xtc-wasm/`) also unconditionally supports gzip response
decoding, so there's no uncompressed fallback kept alongside — it would
just double this header's size for a case that doesn't occur in practice.

Regenerate any time either `.html` file changes, and commit the
regenerated `PagesData.h` alongside it. CI (`.github/workflows/tests.yml`)
reruns this script and fails if the committed header has drifted from a
fresh run — a "did you forget to regenerate" check, not a publish step.

(The WASM XTC decoder's own `.wasm`/`.js` are a separate pair of
generated assets — see `../wasm/generate_header.py` and
`tools/xtc-wasm/README.md` — following the same gzip-embed pattern.)
