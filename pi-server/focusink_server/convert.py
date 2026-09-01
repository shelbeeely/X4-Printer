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
import uuid
from enum import Enum
from typing import TYPE_CHECKING, Callable, Iterator, Optional

import fitz  # PyMuPDF
from PIL import Image

from .util import sha256_file
from .xtc_writer import ConversionError, XtcMetadata, encode_xtc, prepare_landscape_strip_images, prepare_page_image

if TYPE_CHECKING:
    from .config import Config
    from .db import Database

logger = logging.getLogger("focusink.convert")

MAX_PAGE_COUNT = 500  # 500 * 48000 B (X4 panel-sized XTG page) ~= 24MB worst case

SUPPORTED_MIME_TYPES = {
    "application/pdf",
    "application/postscript",
    "image/jpeg",
    "image/png",
    "image/pwg-raster",
}


class RenderMode(Enum):
    # Whole page fit into the panel, aspect-preserved, letterboxed -- the
    # original and still-default mode (prepare_page_image).
    FIT_PAGE = "fit_page"
    # Page split into panel-native, pre-rotated strips meant to be read
    # with the device turned 90 degrees -- see xtc_writer's
    # prepare_landscape_strip_images for the geometry.
    LANDSCAPE_STRIPS = "landscape_strips"

# ConversionError itself now lives in xtc_writer.py (imported above) to
# avoid a circular import -- xtc_writer's own prepare_landscape_strip_images
# raises it too. Re-imported here so existing `from .convert import
# ConversionError` call sites (e.g. ipp_server.py) keep working unchanged.


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


def render_thumbnail_jpeg(page_image: Image.Image, max_width: int = 160, quality: int = 70) -> bytes:
    """Downscales an already-dithered mode "1" panel page — the exact bitmap
    the X4 itself will show, not an idealized re-render — into a small JPEG
    for the Pi admin console's thumbnail route (see admin_api.py). JPEG
    can't encode mode "1" directly, so convert to "L" (grayscale) first;
    since a 1-bit image only has two gray levels either way, the visual
    result is unchanged, just re-tagged for an encoder that accepts it."""
    img = page_image.convert("L")
    img.thumbnail((max_width, max_width * 2))  # generous height cap; aspect ratio preserved
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=quality)
    return buf.getvalue()


def convert_document_to_xtc(
    document_bytes: bytes,
    mime: str,
    *,
    title: str,
    author: str = "",
    target_width: int,
    target_height: int,
    dpi: int = 150,
    mode: RenderMode = RenderMode.FIT_PAGE,
    on_first_page: Optional[Callable[[Image.Image], None]] = None,
) -> tuple[bytes, int]:
    """Returns (xtc_bytes, page_count). Raises ConversionError on failure.

    on_first_page, if given, is called once with the first *output* image
    (the first prepared page, or the first strip of the first page in
    LANDSCAPE_STRIPS mode) — lets a caller (ipp_server.py) derive a
    thumbnail from the exact bitmap already rendered here, without a second
    PDF render or holding every page past when encode_xtc needs them."""
    prepared_pages: list[Image.Image] = []
    page_count = 0
    for source_page in iter_source_pages(document_bytes, mime, dpi):
        if mode is RenderMode.LANDSCAPE_STRIPS:
            new_images = prepare_landscape_strip_images(source_page, target_width, target_height)
        else:
            new_images = [prepare_page_image(source_page, target_width, target_height)]

        for prepared in new_images:
            prepared_pages.append(prepared)
            if page_count == 0 and on_first_page is not None:
                on_first_page(prepared)
            page_count += 1
            if page_count > MAX_PAGE_COUNT:
                raise ConversionError(f"document exceeds the maximum supported page count ({MAX_PAGE_COUNT})")

    if page_count == 0:
        raise ConversionError("document produced zero renderable pages")

    xtc_bytes = encode_xtc(prepared_pages, XtcMetadata(title=title, author=author))
    return xtc_bytes, page_count


def ingest_document(
    config: "Config",
    db: "Database",
    *,
    document_bytes: bytes,
    mime: str,
    title: str,
    source: str,
    job_id: Optional[str] = None,
) -> tuple[str, bool]:
    """Converts + persists a submitted document -- the shared core behind
    ipp_server.py's IPP ingestion (source="ipp") and sync_api.py's X4
    direct-upload endpoint (source="x4_upload", docs/protocol.md §1.7).
    Returns (job_id, is_new). Raises ConversionError if the document is
    empty, unsupported, or fails to render.

    When `job_id` is given (the X4 direct-upload path, where the id is
    generated on-device so it lines up with a Print/Keep/Delete approval
    the device may already have queued against it — see
    docs/architecture.md's direct-upload section) and a row with that id
    already exists, this is a no-op that returns the existing id with
    is_new=False: a retried upload after a dropped connection becomes
    cheap rather than repeating the conversion pass and duplicating files
    on disk. This guards against a duplicate *row*/duplicate conversion
    work on retry only -- it does not by itself guard against a duplicate
    *print*, which is a separate risk from two different devices'
    independent approvals for the same job; see printer_forward.py's
    claim_job_for_finalization for that."""
    if not document_bytes:
        raise ConversionError("empty document body")
    if mime not in SUPPORTED_MIME_TYPES:
        raise ConversionError(f"unsupported document type {mime!r}")

    if job_id is not None and db.get_job(job_id) is not None:
        return job_id, False

    thumbnail_bytes_holder: list[bytes] = []

    def _try_render_thumbnail(img):
        try:
            thumbnail_bytes_holder.append(render_thumbnail_jpeg(img))
        except Exception as exc:  # noqa: BLE001 - must never block job ingestion
            logger.warning("thumbnail generation failed for job %r: %s", title, exc)

    xtc_bytes, page_count = convert_document_to_xtc(
        document_bytes,
        mime,
        title=title,
        target_width=config.panel_width,
        target_height=config.panel_height,
        dpi=config.render_dpi,
        on_first_page=_try_render_thumbnail,
    )

    # Landscape-strip rendering is best-effort -- see the identical comment
    # this mirrors in the pre-extraction ipp_server.py history: a failure
    # here must never block ingestion, the job just ships without a
    # landscape variant.
    landscape_xtc_bytes: Optional[bytes] = None
    landscape_page_count = 0
    try:
        landscape_xtc_bytes, landscape_page_count = convert_document_to_xtc(
            document_bytes,
            mime,
            title=title,
            target_width=config.panel_width,
            target_height=config.panel_height,
            dpi=config.render_dpi,
            mode=RenderMode.LANDSCAPE_STRIPS,
        )
    except ConversionError as exc:
        logger.warning("landscape-strip conversion skipped for job %r: %s", title, exc)

    resolved_id = job_id or uuid.uuid4().hex
    ext = {"application/pdf": "pdf", "image/jpeg": "jpg", "image/png": "png"}.get(mime, "bin")
    original_path = config.originals_dir / f"{resolved_id}.{ext}"
    original_path.write_bytes(document_bytes)
    xtc_path = config.xtc_dir / f"{resolved_id}.xtc"
    xtc_path.write_bytes(xtc_bytes)

    landscape_xtc_path = config.xtc_dir / f"{resolved_id}_landscape.xtc"
    if landscape_xtc_bytes is not None:
        landscape_xtc_path.write_bytes(landscape_xtc_bytes)

    thumbnail_path = config.thumbnails_dir / f"{resolved_id}.jpg"
    if thumbnail_bytes_holder:
        thumbnail_path.write_bytes(thumbnail_bytes_holder[0])

    stored_id, is_new = db.insert_job(
        title=title,
        source=source,
        original_path=str(original_path),
        original_mime=mime,
        original_bytes=len(document_bytes),
        xtc_path=str(xtc_path),
        xtc_bytes=len(xtc_bytes),
        xtc_sha256=sha256_file(xtc_path),
        page_count=page_count,
        thumbnail_path=str(thumbnail_path) if thumbnail_bytes_holder else "",
        xtc_landscape_path=str(landscape_xtc_path) if landscape_xtc_bytes is not None else "",
        xtc_landscape_bytes=len(landscape_xtc_bytes) if landscape_xtc_bytes is not None else 0,
        xtc_landscape_sha256=sha256_file(landscape_xtc_path) if landscape_xtc_bytes is not None else "",
        xtc_landscape_page_count=landscape_page_count,
        job_id=job_id,
    )
    logger.info(
        "ingested job %s %r (%d pages, %d bytes original, source=%s, is_new=%s)",
        stored_id, title, page_count, len(document_bytes), source, is_new,
    )
    return stored_id, is_new
