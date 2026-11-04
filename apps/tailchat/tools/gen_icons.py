#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Draws the Chat app's pictures into LVGL fonts.

    apps/tailchat/tools/gen_icons.py

Writes tailchat/tailchat_icons_{14,20}.c, antialiased (4 bits a pixel) for
colour screens, and their _mono cuts, one bit a pixel for e-paper. Each
picture is a glyph, so a label or a table cell draws it in its own colour,
which is how a chosen row and the black bubbles invert it. tailchat_icons.h
names them:

    U+E000  a microphone        the composer, empty
    U+E001  an arrow up         the composer: Send
    U+E002  a clock             a message waiting to go
    U+E003  a tick              delivered
    U+E004  two ticks           read
    U+E005  a triangle          Play
    U+E006  a square            Stop
    U+E007  an arrow back       Reply
    U+E008  a person            Contacts
    U+E009  a person and a +    Add contact
    U+E00A  waves from a point  Look for chat peers
    U+E00B  a speech bubble     Chats
    U+E00C  a key               the fingerprint
    U+2764  a heart, solid      Noto Emoji's is hatched, which one bit a
                                pixel turns to stripes
    U+200D, U+FE0F, U+1F3FB..U+1F3FF
                                nothing, and no width: the joiner, the
                                emoji selector and skin tones, which a
                                font of one colour has no use for

The 14 px cut stands in the Chat app's text font after Montserrat 14 (its
line of 16 px), the 20 px one on its round buttons. Each is drawn on a grid
of 16 units, sampled 8 x 8 times a pixel. Python's standard library only.
"""

from __future__ import annotations

import math
from pathlib import Path

SS = 8              # samples a pixel, each way
GRID = 16.0         # the units each picture is drawn in


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


def polyline(points: list[tuple[float, float]], width: float):
    parts = [segment(*a, *b, width) for a, b in zip(points, points[1:])]
    return lambda x, y: any(p(x, y) for p in parts)


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


def rounded_rect(x1: float, y1: float, x2: float, y2: float, r: float):
    def inside(x: float, y: float) -> bool:
        if not (x1 <= x <= x2 and y1 <= y <= y2):
            return False
        cx = min(max(x, x1 + r), x2 - r)
        cy = min(max(y, y1 + r), y2 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r

    return inside


def union(*shapes):
    return lambda x, y: any(shape(x, y) for shape in shapes)


def minus(shape, cut):
    return lambda x, y: shape(x, y) and not cut(x, y)


def mic():
    arc = lambda x, y: y >= 7.5 and ring(8, 7.5, 6.0, 4.5)(x, y)
    return union(rounded_rect(5.3, 0.8, 10.7, 10.4, 2.7), arc,
                 segment(8, 13.2, 8, 15.2, 1.6), segment(5, 15.2, 11, 15.2, 1.6))


def send():
    return union(segment(8, 14.6, 8, 3.2, 2.4), polyline([(2.6, 7.6), (8, 2.2), (13.4, 7.6)], 2.4))


def clock():
    return union(ring(8, 8, 7.4, 5.8), segment(8, 8, 8, 3.9, 1.6), segment(8, 8, 11.2, 8, 1.6))


def tick():
    return polyline([(1.8, 8.6), (5.8, 12.6), (14.2, 4.0)], 2.2)


def ticks():
    first = polyline([(0.6, 8.8), (4.2, 12.4), (10.6, 4.6)], 1.9)
    second = polyline([(7.0, 11.0), (8.4, 12.4), (15.4, 4.6)], 1.9)
    return union(first, second)


def play():
    return polygon([(3.6, 1.6), (14.2, 8.0), (3.6, 14.4)])


def stop():
    return rounded_rect(3.0, 3.0, 13.0, 13.0, 1.2)


def reply():
    head = polygon([(0.6, 6.4), (6.4, 1.0), (6.4, 11.8)])
    shaft = segment(5.0, 6.4, 9.0, 6.4, 2.4)
    bend = lambda x, y: x >= 9.0 and y >= 6.4 and ring(9.0, 12.6, 7.4, 5.0)(x, y)
    return union(head, shaft, bend)


def person(cx: float = 8.0, scale: float = 1.0):
    head = circle(cx, 4.6 * scale + (1 - scale) * 6, 3.4 * scale)
    hy = 4.6 * scale + (1 - scale) * 6
    shoulders = lambda x, y: y <= 15.4 and ((x - cx) / (6.9 * scale)) ** 2 + ((y - 16.2) / (6.8 * scale)) ** 2 <= 1
    neck_gap = circle(cx, hy, 4.7 * scale)
    return union(head, minus(shoulders, neck_gap))


def person_add():
    return union(person(6.2, 0.86), segment(13.2, 1.6, 13.2, 8.4, 1.8), segment(9.8, 5.0, 16.0, 5.0, 1.8))


def radar():
    def wedge(x: float, y: float) -> bool:
        return y <= 11.0 and abs(x - 8.0) <= (11.0 - y) * 1.25 + 0.4

    waves = lambda x, y: wedge(x, y) and (ring(8, 11, 5.6, 4.0)(x, y) or ring(8, 11, 9.6, 8.0)(x, y))
    return union(circle(8, 11, 2.0), waves)


def chat():
    body = minus(rounded_rect(0.8, 1.4, 15.2, 11.6, 3.6), rounded_rect(2.4, 3.0, 13.6, 10.0, 2.2))
    tail = polygon([(3.4, 10.4), (3.4, 15.2), (8.2, 10.4)])
    return union(body, minus(tail, rounded_rect(2.4, 3.0, 13.6, 10.0, 2.2)))


def key():
    return union(ring(5.0, 8.0, 4.4, 2.4), segment(8.6, 8.0, 15.2, 8.0, 2.0),
                 segment(12.4, 8.0, 12.4, 11.4, 1.8), segment(14.6, 8.0, 14.6, 10.6, 1.8))


def heart():
    return union(circle(4.9, 5.9, 3.7), circle(11.1, 5.9, 3.7),
                 polygon([(1.4, 7.0), (8.0, 14.6), (14.6, 7.0), (8.0, 4.4)]))


# Each: its codepoint, its name, and what is inside it; None draws nothing.
GLYPHS = [
    (0x200D, "zero width joiner", None),
    (0x2764, "heart", heart()),
    (0xE000, "mic", mic()),
    (0xE001, "send", send()),
    (0xE002, "clock", clock()),
    (0xE003, "tick", tick()),
    (0xE004, "ticks", ticks()),
    (0xE005, "play", play()),
    (0xE006, "stop", stop()),
    (0xE007, "reply", reply()),
    (0xE008, "person", person()),
    (0xE009, "person add", person_add()),
    (0xE00A, "radar", radar()),
    (0xE00B, "chat", chat()),
    (0xE00C, "key", key()),
    (0xFE0F, "emoji selector", None),
] + [(0x1F3FB + i, f"skin tone {i + 1}", None) for i in range(5)]


def coverage(shape, size: int) -> list[list[float]]:
    rows = []
    unit = GRID / size
    for py in range(size):
        row = []
        for px in range(size):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if shape((px + (sx + 0.5) / SS) * unit, (py + (sy + 0.5) / SS) * unit):
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


def runs(codepoints: list[int]) -> list[tuple[int, int, int]]:
    """Consecutive codepoints as (first, length, first glyph id)."""
    out = []
    for gid, cp in enumerate(codepoints, start=1):
        if out and out[-1][0] + out[-1][1] == cp:
            first, length, start = out[-1]
            out[-1] = (first, length + 1, start)
        else:
            out.append((cp, 1, gid))
    return out


def write_font(path: Path, name: str, size: int, line_height: int, base_line: int, bpp: int, drawn) -> None:
    bitmap, dscs, art = [], [], []
    for cp, label, cov in drawn:
        if cov is None:
            dscs.append((len(bitmap), cp, label, 0))
            art.append([])
            continue
        if bpp == 1:
            pixels = [1 if c >= 0.5 else 0 for row in cov for c in row]
        else:
            pixels = [min(15, int(round(c * 15))) for row in cov for c in row]
        dscs.append((len(bitmap), cp, label, size))
        art.append(pixels)
        bitmap.extend(pack(pixels, bpp))

    # The picture sits as Montserrat's capitals do: its middle on theirs.
    ofs_y = -(size - (line_height - base_line)) // 2 - base_line // 2
    lines = [
        "/*******************************************************************************",
        f" * Size: {size} px",
        f" * Bpp: {bpp}",
        " * Drawn by apps/tailchat/tools/gen_icons.py: edit that, not this.",
        " ******************************************************************************/",
        "",
        '#include "lvgl.h"',
        "",
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {",
    ]
    for (start, cp, label, width), pixels in zip(dscs, art):
        if not width:
            continue
        end = start + (width * size * bpp + 7) // 8
        lines.append(f"    /* U+{cp:04X} {label} */")
        for row in range(size):
            shown = "".join(" " if p == 0 else ("#" if bpp == 1 or p > 10 else "+" if p > 4 else ".")
                            for p in pixels[row * width:(row + 1) * width])
            lines.append(f"    /* |{shown}| */")
        chunk = bitmap[start:end]
        for i in range(0, len(chunk), 12):
            lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk[i:i + 12]))
    if not bitmap:
        lines.append("    0x00,")
    lines += [
        "};",
        "",
        "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {",
        "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,",
    ]
    for start, cp, label, width in dscs:
        if width:
            lines.append(f"    {{.bitmap_index = {start}, .adv_w = {(width + 2) * 16}, .box_w = {width}, "
                         f".box_h = {size}, .ofs_x = 1, .ofs_y = {ofs_y}}}, /* U+{cp:04X} {label} */")
        else:
            lines.append(f"    {{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}}, "
                         f"/* U+{cp:04X} {label} */")
    cmaps = runs([cp for _, cp, _, _ in dscs])
    lines += ["};", "", "static const lv_font_fmt_txt_cmap_t cmaps[] = {"]
    for first, length, gid in cmaps:
        lines += [
            "    {",
            f"        .range_start = 0x{first:04X}, .range_length = {length}, .glyph_id_start = {gid},",
            "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,",
            "        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY",
            "    },",
        ]
    lines += [
        "};",
        "",
        "static const lv_font_fmt_txt_dsc_t font_dsc = {",
        "    .glyph_bitmap = glyph_bitmap,",
        "    .glyph_dsc = glyph_dsc,",
        "    .cmaps = cmaps,",
        "    .kern_dsc = NULL,",
        "    .kern_scale = 0,",
        f"    .cmap_num = {len(cmaps)},",
        f"    .bpp = {bpp},",
        "    .kern_classes = 0,",
        "    .bitmap_format = 0,",
        "};",
        "",
        f"const lv_font_t {name} = {{",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {line_height},",
        f"    .base_line = {base_line},",
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
    out = Path(__file__).resolve().parent.parent / "tailchat"
    # Montserrat 14's line is 16 px with 3 below the baseline; 18's is 21, 4.
    for size, line_height, base_line in ((14, 16, 3), (20, 22, 4)):
        drawn = [(cp, label, None if shape is None else coverage(shape, size)) for cp, label, shape in GLYPHS]
        write_font(out / f"tailchat_icons_{size}.c", f"tailchat_icons_{size}", size, line_height, base_line, 4, drawn)
        write_font(out / f"tailchat_icons_{size}_mono.c", f"tailchat_icons_{size}_mono", size, line_height,
                   base_line, 1, drawn)


if __name__ == "__main__":
    main()
