"""Document -> XTC conversion pipeline.

Renders one *source* page at a time (PyMuPDF's ``get_pixmap`` on a single
page) and immediately dithers it down to the small (~48KB, panel-sized)
mode "1" form before moving to the next page — so the multi-megabyte
full-resolution RGB pixmap PyMuPDF produces per page is never held for more
than one page at a time, the same "stream, don't buffer the whole document"
discipline the firmware side applies to downloading and paging. The small
dithered pages *are* accumulated in a list before the final container write
(the XTC index needs the total page count up front — see
``xtc_writer.encode_xtc``), which is why ``MAX_PAGE_COUNT`` below caps
worst-case memory (48000 B/page * cap) to something a Pi Zero W's 512MB,
shared with the OS/CUPS/everything else, can absorb.
"""

from __future__ import annotations

import io
import logging
from typing import Iterator

import fitz  # PyMuPDF
from PIL import Image

from .xtc_writer import XtcMetadata, encode_xtc, prepare_page_image

logger = logging.getLogger("xteink.convert")

MAX_PAGE_COUNT = 500  # 500 * 48000 B (X4 panel-sized XTG page) ~= 24MB worst case

SUPPORTED_MIME_TYPES = {
    "application/pdf",
    "application/postscript",
    "image/jpeg",
    "image/png",
    "image/pwg-raster",
}


class ConversionError(Exception):
    pass


def _iter_pdf_pages(document_bytes: bytes, dpi: int) -> Iterator[Image.Image]:
    try:
        doc = fitz.open(stream=document_bytes, filetype="pdf")
    except Exception as exc:  # noqa: BLE001 - PyMuPDF raises its own exception types
        raise ConversionError(f"failed to open PDF: {exc}") from exc
    try:
        if doc.page_count <= 0:
            raise ConversionError("PDF has zero pages")
        zoom = dpi / 72.0
        matrix = fitz.Matrix(zoom, zoom)
        for index in range(doc.page_count):
            page = doc.load_page(index)
            pix = page.get_pixmap(matrix=matrix, alpha=False)
            img = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
            yield img
            # Drop references promptly; PyMuPDF pixmaps are otherwise held
            # until the C-level object is collected.
            del pix, img
    finally:
        doc.close()


def _iter_image_pages(document_bytes: bytes) -> Iterator[Image.Image]:
    try:
        img = Image.open(io.BytesIO(document_bytes))
        img.load()
    except Exception as exc:  # noqa: BLE001 - Pillow raises its own exception types
        raise ConversionError(f"failed to open image: {exc}") from exc
    yield img


def iter_source_pages(document_bytes: bytes, mime: str, dpi: int) -> Iterator[Image.Image]:
    if mime == "application/pdf":
        yield from _iter_pdf_pages(document_bytes, dpi)
    elif mime in ("image/jpeg", "image/png"):
        yield from _iter_image_pages(document_bytes)
    elif mime == "application/postscript":
        # Ghostscript-free path is out of scope for this prototype; document
        # formats accepted over IPP are PDF/JPEG/PNG (see ipp_server.py's
        # document-format-supported), so this should not normally be hit.
        raise ConversionError("PostScript input is not supported by this converter build")
    else:
        raise ConversionError(f"unsupported document MIME type: {mime}")


def convert_document_to_xtc(
    document_bytes: bytes,
    mime: str,
    *,
    title: str,
    author: str = "",
    target_width: int,
    target_height: int,
    dpi: int = 150,
) -> tuple[bytes, int]:
    """Returns (xtc_bytes, page_count). Raises ConversionError on failure."""
    prepared_pages: list[Image.Image] = []
    page_count = 0
    for source_page in iter_source_pages(document_bytes, mime, dpi):
        prepared_pages.append(prepare_page_image(source_page, target_width, target_height))
        page_count += 1
        if page_count > MAX_PAGE_COUNT:
            raise ConversionError(f"document exceeds the maximum supported page count ({MAX_PAGE_COUNT})")

    if page_count == 0:
        raise ConversionError("document produced zero renderable pages")

    xtc_bytes = encode_xtc(prepared_pages, XtcMetadata(title=title, author=author))
    return xtc_bytes, page_count
