import math
import struct

import pytest
from PIL import Image

from focusink_server.xtc_writer import (
    XTC_HEADER_SIZE,
    ConversionError,
    XtcMetadata,
    encode_xtc,
    encode_xtg_page,
    prepare_landscape_strip_images,
    prepare_page_image,
)


def test_prepare_page_image_produces_exact_target_size():
    src = Image.new("RGB", (300, 900), "white")
    out = prepare_page_image(src, 800, 480)
    assert out.mode == "1"
    assert out.size == (800, 480)


def test_encode_xtg_page_header_and_data_size():
    img = Image.new("1", (800, 480), 1)
    xtg = encode_xtg_page(img)
    mark, width, height, color_mode, compression, data_size = struct.unpack_from("<IHHBBI", xtg, 0)
    assert mark == 0x00475458
    assert (width, height) == (800, 480)
    assert color_mode == 0
    assert compression == 0
    assert data_size == (800 // 8) * 480
    assert len(xtg) == 22 + data_size


def test_encode_xtc_header_offsets_and_roundtrip():
    pages = [prepare_page_image(Image.new("RGB", (800, 480), "white"), 800, 480) for _ in range(3)]
    data = encode_xtc(pages, XtcMetadata(title="My Doc", author="Someone"))

    (mark, version, page_count, read_dir, has_meta, has_thumb, has_chap, cur_page,
     meta_off, idx_off, data_off, thumb_off, chap_off) = struct.unpack_from("<IHHBBBBIQQQQQ", data, 0)

    assert mark == 0x00435458  # "XTC\0"
    assert version == 0x0100
    assert page_count == 3
    assert has_meta == 1
    assert has_thumb == 0
    assert has_chap == 0
    assert meta_off == XTC_HEADER_SIZE
    assert idx_off == meta_off + 256
    assert data_off == idx_off + 3 * 16

    # Title lives at metadata offset 0, null-terminated UTF-8.
    title_bytes = data[meta_off : meta_off + 128]
    assert title_bytes.split(b"\x00", 1)[0] == b"My Doc"
    publisher_bytes = data[meta_off + 0xC0 : meta_off + 0xC0 + 32]
    assert publisher_bytes.split(b"\x00", 1)[0] == b"focusink"

    # Every page index entry must point at a valid embedded XTG header.
    for i in range(3):
        off, size, w, h = struct.unpack_from("<QIHH", data, idx_off + i * 16)
        assert (w, h) == (800, 480)
        xtg = data[off : off + size]
        xmark = struct.unpack_from("<I", xtg, 0)[0]
        assert xmark == 0x00475458
        assert len(xtg) == size


def test_encode_xtc_rejects_empty_page_list():
    import pytest

    with pytest.raises(ValueError):
        encode_xtc([], XtcMetadata(title="Empty"))


def test_encode_xtg_page_rejects_non_mode_1():
    import pytest

    with pytest.raises(ValueError):
        encode_xtg_page(Image.new("L", (10, 10)))


def test_prepare_landscape_strip_images_single_strip_for_normal_portrait_page():
    # A typical portrait page (roughly US letter aspect) needs only one
    # strip: width-mapped-to-panel_height scale keeps the rendered height
    # under one panel_width-tall chunk.
    src = Image.new("RGB", (850, 1100), "white")
    strips = prepare_landscape_strip_images(src, 800, 480)
    assert len(strips) == 1
    for strip in strips:
        assert strip.mode == "1"
        assert strip.size == (800, 480)


def test_prepare_landscape_strip_images_splits_tall_page_into_multiple_strips():
    src = Image.new("RGB", (200, 6000), "white")
    strips = prepare_landscape_strip_images(src, 800, 480)
    scale = 480 / 200
    expected_count = math.ceil(round(6000 * scale) / 800)
    assert expected_count > 1  # sanity-check the test fixture actually exercises multi-strip
    assert len(strips) == expected_count
    for strip in strips:
        assert strip.mode == "1"
        assert strip.size == (800, 480)


def test_prepare_landscape_strip_images_raises_past_max_strips():
    src = Image.new("RGB", (200, 6000), "white")  # needs 18 strips at the default scale math
    with pytest.raises(ConversionError):
        prepare_landscape_strip_images(src, 800, 480, max_strips=5)
