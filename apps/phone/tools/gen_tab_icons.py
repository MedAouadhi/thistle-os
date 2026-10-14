#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Draws the Phone app's tab bar pictures into an LVGL font.

    apps/phone/tools/gen_tab_icons.py

Writes phone/phone_icons_18.c, antialiased (4 bits a pixel) for colour
screens, and phone/phone_icons_18_mono.c, one bit a pixel for e-paper. Each
picture is a glyph in the Private Use Area, so a label draws it and takes its
colour from the label, which is how the chosen tab inverts:

    U+E000  a star        Favourites
    U+E001  a clock       Recents
    U+E002  a person      Contacts
    U+E003  ten keys      Keypad

No font at hand has a person or a keypad in a form free to ship, so they are
drawn here from circles, lines and a polygon, sampled 8 x 8 times a pixel.
Python's standard library only.
"""

from __future__ import annotations

import math
from pathlib import Path

SIZE = 18          # each picture's box, and the font's line
SS = 8             # samples a pixel, each way
FIRST = 0xE000


def circle(cx: float, cy: float, r: float):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx: float, cy: float, r_out: float, r_in: float):
    return lambda x, y: r_in * r_in <= (x - cx) ** 2 + (y - cy) ** 2 <= r_out * r_out


def segment(x1: float, y1: float, x2: float, y2: float, width: float):
    dx, dy = x2 - x1, y2 - y1
    length2 = dx * dx + dy * dy

    def inside(x: float, y: float) -> bool:
        t = max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / length2))
        px, py = x1 + t * dx, y1 + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= (width / 2) ** 2

    return inside


def polygon(points: list[tuple[float, float]]):
    def inside(x: float, y: float) -> bool:
        hit = False
        j = len(points) - 1
        for i, (xi, yi) in enumerate(points):
            xj, yj = points[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit

    return inside


def union(*shapes):
    return lambda x, y: any(shape(x, y) for shape in shapes)


def star():
    cx, cy, r_out, r_in = 9.0, 9.9, 9.2, 4.1
    points = []
    for k in range(10):
        r = r_out if k % 2 == 0 else r_in
        a = -math.pi / 2 + k * math.pi / 5
        points.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return polygon(points)


def clock():
    return union(
        ring(9, 9, 8.6, 6.8),
        segment(9, 9, 9, 4.2, 1.8),    # the hour hand, at twelve
        segment(9, 9, 12.6, 9, 1.8),   # the minute hand, at three
    )


def person():
    head = circle(9, 5.2, 3.9)
    # Shoulders: the top of an ellipse, cut off at the bottom of the box.
    shoulders = lambda x, y: y <= 18 and ((x - 9) / 8.2) ** 2 + ((y - 18.2) / 7.4) ** 2 <= 1
    neck_gap = lambda x, y: (x - 9) ** 2 + (y - 5.2) ** 2 <= 5.4 ** 2
    return lambda x, y: head(x, y) or (shoulders(x, y) and not neck_gap(x, y))


def keypad():
    # Seventeen pixels wide, so that each key is three pixels centred on
    # pixels, three apart: 1 2 3 / 4 5 6 / 7 8 9 / 0.
    dots = [circle(x, y, 1.7) for y in (1.5, 6.5, 11.5) for x in (2.5, 8.5, 14.5)]
    dots.append(circle(8.5, 16.5, 1.7))
    return union(*dots)


# Each picture: its name, its width, and what is inside it.
ICONS = (("star", SIZE, star()), ("clock", SIZE, clock()), ("person", SIZE, person()),
         ("keypad", 17, keypad()))


def coverage(shape, width: int) -> list[list[float]]:
    rows = []
    for py in range(SIZE):
        row = []
        for px in range(width):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if shape(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS):
                        hits += 1
            row.append(hits / (SS * SS))
        rows.append(row)
    return rows


def pack(values: list[int], bpp: int) -> list[int]:
    """Pixels into bytes, first pixel in the high bits, rows run together,
    as lv_font_conv writes a plain bitmap."""
    out, acc, nbits = [], 0, 0
    for v in values:
        acc = (acc << bpp) | v
        nbits += bpp
        if nbits == 8:
            out.append(acc)
            acc, nbits = 0, 0
    if nbits:
        out.append(acc << (8 - nbits))
    return out


def write_font(path: Path, name: str, bpp: int, pictures) -> None:
    bitmap, dscs, art = [], [], []
    for label, width, cov in pictures:
        if bpp == 1:
            pixels = [1 if c >= 0.5 else 0 for row in cov for c in row]
        else:
            pixels = [min(15, int(round(c * 15))) for row in cov for c in row]
        dscs.append((len(bitmap), label, width))
        art.append(pixels)
        bitmap.extend(pack(pixels, bpp))

    lines = [
        "/*******************************************************************************",
        f" * Size: {SIZE} px",
        f" * Bpp: {bpp}",
        " * Drawn by apps/phone/tools/gen_tab_icons.py: edit that, not this.",
        " ******************************************************************************/",
        "",
        '#include "lvgl.h"',
        "",
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {",
    ]
    for (start, label, width), pixels in zip(dscs, art):
        end = start + (width * SIZE * bpp + 7) // 8
        lines.append(f"    /* {label} */")
        for row in range(SIZE):
            drawn = "".join(" " if p == 0 else ("#" if bpp == 1 or p > 10 else "+" if p > 4 else ".")
                            for p in pixels[row * width:(row + 1) * width])
            lines.append(f"    /* |{drawn}| */")
        chunk = bitmap[start:end]
        for i in range(0, len(chunk), 12):
            lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk[i:i + 12]))
    lines += [
        "};",
        "",
        "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {",
        "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,",
    ]
    for start, label, width in dscs:
        lines.append(f"    {{.bitmap_index = {start}, .adv_w = {(width + 2) * 16}, .box_w = {width}, "
                     f".box_h = {SIZE}, .ofs_x = 1, .ofs_y = 0}}, /* {label} */")
    lines += [
        "};",
        "",
        "static const lv_font_fmt_txt_cmap_t cmaps[] = {",
        "    {",
        f"        .range_start = 0x{FIRST:04X}, .range_length = {len(dscs)}, .glyph_id_start = 1,",
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,",
        "        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY",
        "    }",
        "};",
        "",
        "static const lv_font_fmt_txt_dsc_t font_dsc = {",
        "    .glyph_bitmap = glyph_bitmap,",
        "    .glyph_dsc = glyph_dsc,",
        "    .cmaps = cmaps,",
        "    .kern_dsc = NULL,",
        "    .kern_scale = 0,",
        "    .cmap_num = 1,",
        f"    .bpp = {bpp},",
        "    .kern_classes = 0,",
        "    .bitmap_format = 0,",
        "};",
        "",
        f"const lv_font_t {name} = {{",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {SIZE},",
        "    .base_line = 0,",
        "    .subpx = LV_FONT_SUBPX_NONE,",
        "    .underline_position = 0,",
        "    .underline_thickness = 1,",
        "    .dsc = &font_dsc,",
        "    .fallback = NULL,",
        "    .user_data = NULL,",
        "};",
        "",
    ]
    path.write_text("\n".join(lines))
    print(f"wrote {path}")


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "phone"
    pictures = [(label, width, coverage(shape, width)) for label, width, shape in ICONS]
    write_font(out / "phone_icons_18.c", "phone_icons_18", 4, pictures)
    write_font(out / "phone_icons_18_mono.c", "phone_icons_18_mono", 1, pictures)


if __name__ == "__main__":
    main()
