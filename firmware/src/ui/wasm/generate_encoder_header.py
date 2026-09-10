#!/usr/bin/env python3
"""Turns the committed xtc_encoder.wasm / xtc_encoder.js (built by
tools/xtc-wasm/build_encoder.sh) into firmware/src/ui/XtcEncoderWasmData.h,
which WebUiServer.cpp #includes to serve both over HTTP without any
filesystem access on the device — same embed pipeline as
generate_header.py (the decoder's own generator), one module at a time.

Regenerate any time xtc_encoder.cpp changes and is rebuilt:
    bash tools/xtc-wasm/build_encoder.sh
    python3 firmware/src/ui/wasm/generate_encoder_header.py
CI (.github/workflows/tests.yml) rebuilds from source and fails if this
generated header has drifted from what's committed.

gzip.compress(..., mtime=0) (not the default, which embeds the current
wall-clock time) keeps the output byte-for-byte reproducible across
runs/machines, so the CI regenerate-check only fails on an actual content
change.
"""

from __future__ import annotations

import gzip
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
WASM_PATH = HERE / "xtc_encoder.wasm"
JS_PATH = HERE / "xtc_encoder.js"
OUT_PATH = HERE.parent / "XtcEncoderWasmData.h"


def to_byte_array(data: bytes) -> str:
    rows = []
    row = []
    for b in data:
        row.append(f"0x{b:02x}")
        if len(row) == 20:
            rows.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        rows.append("    " + ", ".join(row) + ",")
    return "\n".join(rows)


def main() -> None:
    wasm_bytes = WASM_PATH.read_bytes()
    js_bytes = JS_PATH.read_bytes()

    wasm_gz = gzip.compress(wasm_bytes, compresslevel=9, mtime=0)
    js_gz = gzip.compress(js_bytes, compresslevel=9, mtime=0)

    header = f"""#pragma once
// GENERATED FILE — do not hand-edit. Produced by
// firmware/src/ui/wasm/generate_encoder_header.py from xtc_encoder.wasm /
// xtc_encoder.js (built by tools/xtc-wasm/build_encoder.sh from
// tools/xtc-wasm/xtc_encoder.cpp). Regenerate after rebuilding the WASM
// module; CI checks this file hasn't drifted from a fresh rebuild.

#include <cstddef>

namespace ui {{

// xtc_encoder.wasm: {len(wasm_bytes)} raw bytes -> {len(wasm_gz)} gzip'd ({100 * len(wasm_gz) / len(wasm_bytes):.0f}%).
constexpr unsigned char kXtcEncoderWasmGz[] = {{
{to_byte_array(wasm_gz)}
}};
constexpr size_t kXtcEncoderWasmGzLen = sizeof(kXtcEncoderWasmGz);

// xtc_encoder.js: {len(js_bytes)} raw bytes -> {len(js_gz)} gzip'd ({100 * len(js_gz) / len(js_bytes):.0f}%).
constexpr unsigned char kXtcEncoderJsGz[] = {{
{to_byte_array(js_gz)}
}};
constexpr size_t kXtcEncoderJsGzLen = sizeof(kXtcEncoderJsGz);

}}  // namespace ui
"""
    OUT_PATH.write_text(header)
    print(f"wrote {OUT_PATH} (wasm {len(wasm_bytes)}->{len(wasm_gz)}, js {len(js_bytes)}->{len(js_gz)})")


if __name__ == "__main__":
    main()
