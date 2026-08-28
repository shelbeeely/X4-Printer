#!/usr/bin/env python3
"""Turns login.html / joblist.html (the human-edited source of the X4's two
on-device web UI pages) into firmware/src/ui/PagesData.h — gzip-compressed
byte arrays that WebUiServer.cpp serves directly with a Content-Encoding:
gzip header, decoded for free by the requesting browser. Real measurement
on the current pages: 2820/7435 raw bytes -> 1415/3044 gzip'd (50%/41% of
original) — meaningful savings on both flash usage (these are compiled
into the firmware image) and the bytes actually sent over the X4's own
constrained hotspot AP.

Same generated-but-committed pattern as firmware/src/ui/wasm/
generate_header.py: edit the .html source, rerun this script, commit the
regenerated header alongside it. CI (.github/workflows/tests.yml)
rebuilds from source and fails if this header has drifted from what's
committed.

gzip.compress(..., mtime=0) is used (not the default, which embeds the
current wall-clock time) so the output is byte-for-byte reproducible
across runs/machines — otherwise the CI regenerate-check would fail on
every run for a reason that has nothing to do with the actual content.
"""

from __future__ import annotations

import gzip
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
OUT_PATH = HERE.parent / "PagesData.h"

PAGES = [
    ("login.html", "kLoginPageHtmlGz"),
    ("joblist.html", "kJobListPageHtmlGz"),
]


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
    sections = []
    for filename, const_name in PAGES:
        raw = (HERE / filename).read_bytes()
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        sections.append(f"""// {filename}: {len(raw)} raw bytes -> {len(gz)} gzip'd ({100 * len(gz) / len(raw):.0f}%).
constexpr unsigned char {const_name}[] = {{
{to_byte_array(gz)}
}};
constexpr size_t {const_name}Len = sizeof({const_name});
""")

    header = f"""#pragma once
// GENERATED FILE — do not hand-edit. Produced by
// firmware/src/ui/pages/generate_pages_header.py from login.html /
// joblist.html in the same directory (the human-edited source of truth).
// Regenerate after editing either page; CI checks this file hasn't
// drifted from a fresh run of that script.
//
// Served with a "Content-Encoding: gzip" header (see
// WebUiServer.cpp's sendGzip() helper) — every browser capable of
// running this UI's client-side WASM decode also unconditionally
// supports gzip response decoding, so there's no plain-text fallback
// variant kept here (it would just double this header's size for a
// case that doesn't occur in practice).

#include <cstddef>

namespace ui {{

{"".join(sections)}}}  // namespace ui
"""
    OUT_PATH.write_text(header)
    print(f"wrote {OUT_PATH}")


if __name__ == "__main__":
    main()
