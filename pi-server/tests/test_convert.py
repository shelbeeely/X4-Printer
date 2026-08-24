import struct

import pytest

from tests.conftest import make_test_pdf, make_test_png
from xteink_print_server.convert import ConversionError, convert_document_to_xtc


def test_convert_pdf_produces_expected_page_count():
    pdf = make_test_pdf(pages=3)
    xtc_bytes, page_count = convert_document_to_xtc(
        pdf, "application/pdf", title="Three Pager", target_width=800, target_height=480
    )
    assert page_count == 3
    mark, version, count = struct.unpack_from("<IHH", xtc_bytes, 0)
    assert mark == 0x00435458
    assert count == 3


def test_convert_png_single_page():
    png = make_test_png()
    xtc_bytes, page_count = convert_document_to_xtc(
        png, "image/png", title="A Photo", target_width=800, target_height=480
    )
    assert page_count == 1


def test_convert_rejects_unsupported_mime():
    with pytest.raises(ConversionError):
        convert_document_to_xtc(b"garbage", "application/zip", title="x", target_width=800, target_height=480)


def test_convert_rejects_empty_pdf_bytes():
    with pytest.raises(ConversionError):
        convert_document_to_xtc(b"not a pdf", "application/pdf", title="x", target_width=800, target_height=480)
