"""Pure-Python encoder for the XTC container / XTG bitmap format.

Implements the byte-exact layout documented in docs/xtc-format.md (itself a
distillation of the public phrozen/xtx spec). This module has **no**
dependency on the phrozen/xtx Go module — it is a from-scratch
implementation so the Pi doesn't need a Go toolchain, and it deliberately
only emits the subset this project needs: monochrome (XTG) pages, no
grayscale (XTH) pages, no thumbnails, no chapters, always-present metadata.
See docs/xtc-format.md for the rationale.

All multi-byte fields are little-endian, matching the spec.
"""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass
from typing import Iterable, Sequence

from PIL import Image

MAGIC_XTG = 0x00475458
MAGIC_XTC = 0x00435458

XTG_HEADER_SIZE = 22
XTC_HEADER_SIZE = 56
METADATA_SIZE = 256
INDEX_ENTRY_SIZE = 16

META_TITLE_SIZE = 128
META_AUTHOR_SIZE = 64
META_PUBLISHER_SIZE = 32
META_LANGUAGE_SIZE = 16

PUBLISHER_TAG = "xteink-print-inbox"


def _row_bytes(width: int) -> int:
    return (width + 7) // 8


def encode_xtg_page(image: Image.Image) -> bytes:
    """Encode a single already-sized image as one complete XTG file (22-byte
    header + packed 1bpp data). ``image`` must already be the exact target
    resolution — this function does not resize.

    Pillow's mode "1" ``tobytes()`` packs rows MSB-first, 8 pixels/byte,
    zero-padded to a byte boundary per row, with bit=1 meaning a white
    (non-zero) source pixel and bit=0 meaning black — which is byte-for-byte
    the XTG layout in docs/xtc-format.md, so encoding is a direct pack with
    no bit-twiddling of our own.
    """
    if image.mode != "1":
        raise ValueError("encode_xtg_page requires an already-dithered mode '1' image")
    width, height = image.size
    data = image.tobytes()
    expected = _row_bytes(width) * height
    if len(data) != expected:
        raise ValueError(f"XTG data size mismatch: got {len(data)} bytes, expected {expected}")

    header = struct.pack(
        "<IHHBBIQ",
        MAGIC_XTG,
        width,
        height,
        0,  # colorMode: monochrome
        0,  # compression: none
        expected,
        0,  # md5: unused
    )
    assert len(header) == XTG_HEADER_SIZE
    return header + data


@dataclass
class XtcMetadata:
    title: str
    author: str = ""
    create_time: int | None = None
    language: str = "en-US"


def _pack_fixed_string(value: str, size: int) -> bytes:
    encoded = value.encode("utf-8")[: size - 1]
    return encoded + b"\x00" * (size - len(encoded))


def encode_xtc(pages: Sequence[Image.Image], metadata: XtcMetadata) -> bytes:
    """Encode a full XTC container from a sequence of pre-sized, pre-dithered
    mode "1" page images. Always writes metadata; never writes thumbnails or
    chapters (this project doesn't use either — see docs/xtc-format.md)."""
    if not pages:
        raise ValueError("XTC container must have at least one page")

    xtg_pages: list[bytes] = [encode_xtg_page(p) for p in pages]
    page_count = len(xtg_pages)

    offset = XTC_HEADER_SIZE
    metadata_offset = offset
    offset += METADATA_SIZE

    index_offset = offset
    offset += page_count * INDEX_ENTRY_SIZE

    data_offset = offset
    page_offsets = []
    pos = data_offset
    for xtg in xtg_pages:
        page_offsets.append(pos)
        pos += len(xtg)

    header = struct.pack(
        "<IHHBBBBIQQQQQ",
        MAGIC_XTC,
        0x0100,  # version 1.0
        page_count,
        0,  # readDirection: left-to-right
        1,  # hasMetadata
        0,  # hasThumbnails
        0,  # hasChapters
        0,  # currentPage
        metadata_offset,
        index_offset,
        data_offset,
        0,  # thumbOffset (unused)
        0,  # chapterOffset (unused)
    )
    assert len(header) == XTC_HEADER_SIZE

    meta_buf = bytearray(METADATA_SIZE)
    meta_buf[0:META_TITLE_SIZE] = _pack_fixed_string(metadata.title, META_TITLE_SIZE)
    meta_buf[0x80 : 0x80 + META_AUTHOR_SIZE] = _pack_fixed_string(metadata.author, META_AUTHOR_SIZE)
    meta_buf[0xC0 : 0xC0 + META_PUBLISHER_SIZE] = _pack_fixed_string(PUBLISHER_TAG, META_PUBLISHER_SIZE)
    meta_buf[0xE0 : 0xE0 + META_LANGUAGE_SIZE] = _pack_fixed_string(metadata.language, META_LANGUAGE_SIZE)
    struct.pack_into("<IHH", meta_buf, 0xF0, metadata.create_time or int(time.time()), 0xFFFF, 0)
    # bytes 0xF8-0xFF stay zero (reserved)

    index_buf = bytearray()
    for xtg, page_offset in zip(xtg_pages, page_offsets):
        # page dims live in the XTG's own header at bytes [4:8]; re-read
        # rather than re-deriving so the index can never drift from the data.
        width, height = struct.unpack_from("<HH", xtg, 4)
        index_buf += struct.pack("<QIHH", page_offset, len(xtg), width, height)

    out = bytearray()
    out += header
    out += meta_buf
    out += index_buf
    for xtg in xtg_pages:
        out += xtg
    return bytes(out)


def prepare_page_image(image: Image.Image, target_width: int, target_height: int, background: str = "white") -> Image.Image:
    """Resize (contain, preserving aspect ratio, letterboxed) and dither a
    source page image to a mode "1" image of exactly target_width x
    target_height, ready for encode_xtg_page(). Kept separate from the
    encoder so callers (convert.py, tests) can swap in a pre-made mode "1"
    image without going through PIL's resize/dither path."""
    img = image.convert("L")
    src_w, src_h = img.size
    scale = min(target_width / src_w, target_height / src_h)
    new_w = max(1, round(src_w * scale))
    new_h = max(1, round(src_h * scale))
    resized = img.resize((new_w, new_h), Image.LANCZOS)

    canvas = Image.new("L", (target_width, target_height), 255 if background == "white" else 0)
    paste_x = (target_width - new_w) // 2
    paste_y = (target_height - new_h) // 2
    canvas.paste(resized, (paste_x, paste_y))

    # Floyd-Steinberg dithering (PIL's default for convert("1")) gives much
    # better print-document legibility than a hard threshold.
    return canvas.convert("1")


def iter_prepared_pages(
    images: Iterable[Image.Image], target_width: int, target_height: int
) -> Iterable[Image.Image]:
    for img in images:
        yield prepare_page_image(img, target_width, target_height)
