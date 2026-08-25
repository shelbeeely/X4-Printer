#!/usr/bin/env python3
"""Turns the committed xtc_decoder.wasm / xtc_decoder.js (built by
tools/xtc-wasm/build.sh) into firmware/src/ui/XtcDecoderWasmData.h, which
WebUiServer.cpp #includes to serve both over HTTP without any filesystem
access on the device (everything the firmware serves is flash-resident,
same as the existing HTML templates in WebUiServer.cpp).

The .wasm is binary, so it's embedded as a byte array (same reasoning the
design-canvas skill uses for images: binary data can't be a text literal).
The .js glue is plain text, so it's embedded as a C++ raw string literal
like the existing kLoginPageHtml/kJobListPageHtml constants already are.

Regenerate any time xtc_decoder.cpp changes and is rebuilt:
    bash tools/xtc-wasm/build.sh
    python3 firmware/src/ui/wasm/generate_header.py
CI (.github/workflows/tests.yml) rebuilds from source and fails if this
generated header has drifted from what's committed.
"""

from __future__ import annotations

import pathlib

HERE = pathlib.Path(__file__).resolve().parent
WASM_PATH = HERE / "xtc_decoder.wasm"
JS_PATH = HERE / "xtc_decoder.js"
OUT_PATH = HERE.parent / "XtcDecoderWasmData.h"

RAW_STRING_DELIM = "WASMJS"


def main() -> None:
    wasm_bytes = WASM_PATH.read_bytes()
    js_text = JS_PATH.read_text()

    terminator = ")" + RAW_STRING_DELIM + '"'
    if terminator in js_text:  # pragma: no cover - defensive
        raise SystemExit(f"xtc_decoder.js contains the raw-string terminator {terminator!r} — pick a new delimiter")

    hex_bytes = ", ".join(f"0x{b:02x}" for b in wasm_bytes)
    # Wrap at a reasonable line width so the generated header isn't one
    # giant line (cosmetic only, doesn't affect the compiled output).
    wrapped_lines = []
    row = []
    for i, b in enumerate(wasm_bytes):
        row.append(f"0x{b:02x}")
        if len(row) == 20:
            wrapped_lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        wrapped_lines.append("    " + ", ".join(row) + ",")
    wasm_array_body = "\n".join(wrapped_lines)

    header = f"""#pragma once
// GENERATED FILE — do not hand-edit. Produced by
// firmware/src/ui/wasm/generate_header.py from xtc_decoder.wasm /
// xtc_decoder.js (built by tools/xtc-wasm/build.sh from
// tools/xtc-wasm/xtc_decoder.cpp). Regenerate after rebuilding the WASM
// module; CI checks this file hasn't drifted from a fresh rebuild.

#include <cstddef>

namespace ui {{

constexpr unsigned char kXtcDecoderWasm[] = {{
{wasm_array_body}
}};
constexpr size_t kXtcDecoderWasmLen = sizeof(kXtcDecoderWasm);

constexpr const char* kXtcDecoderJs = R"{RAW_STRING_DELIM}(
{js_text}
){RAW_STRING_DELIM}";

}}  // namespace ui
"""
    OUT_PATH.write_text(header)
    print(f"wrote {OUT_PATH} ({len(wasm_bytes)} wasm bytes, {len(js_text)} js chars)")


if __name__ == "__main__":
    main()
