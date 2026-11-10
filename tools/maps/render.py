# SPDX-License-Identifier: BSD-3-Clause
"""Draws the map's tiles: 256 x 256, one bit a pixel, north up, with no
anti-aliasing anywhere, for an e-paper panel that shows only black and
white.

Tiles are drawn in blocks of up to 8 x 8 (a metatile) with a margin, then
cut, so a road or a label crossing a tile's edge is drawn whole on both
sides. Every label is placed by its feature's own position, never by the
block's, so neighbouring blocks agree about it; a label crowded out in one
block may still be drawn in its neighbour's margin, which is the one known
seam (docs/maps/map-pack-format.md, "Cartography").

What is drawn, by level (ADR-0017):

    water         z14-z17  horizontal hatching, outlined
    parks         z14-z17  diagonal hatching, outlined
    buildings     z16-z17  a 50 % screen, outlined
    roads         z14-z17  by class: wide roads cased (black edges, white
                           middle), small ones a black line, paths dashed
    place names   z14-z17  shaped by HarfBuzz (through Pillow's raqm), so
                           Arabic is joined and right to left
    road names    z15-z17  along the road
    icons         z16-z17  about a dozen kinds (icons.py); names at z17
"""

from __future__ import annotations

import math
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, features

import icons
from osm_source import MapData
from regions import world_pixel

META = 8          # tiles a side in a metatile
MARGIN = 96       # pixels drawn beyond a metatile's tiles on each side
TILE = 256

# Each road class's (outer width, inner width) by level, in pixels. An inner
# width of 0 is a plain black line; "dash" is a dashed one-pixel line.
ROAD_STYLE = {
    "path":      {16: (1, "dash"), 17: (1, "dash")},
    "service":   {15: (1, 0), 16: (2, 0), 17: (5, 3)},
    "minor":     {14: (1, 0), 15: (1, 0), 16: (6, 4), 17: (9, 6)},
    "tertiary":  {14: (1, 0), 15: (3, 0), 16: (8, 5), 17: (11, 8)},
    "secondary": {14: (2, 0), 15: (4, 2), 16: (9, 6), 17: (12, 9)},
    "major":     {14: (3, 0), 15: (5, 3), 16: (11, 7), 17: (14, 10)},
}
ROAD_ORDER = ("path", "service", "minor", "tertiary", "secondary", "major")
ROAD_LABEL_FROM = {"minor": 16, "tertiary": 16, "secondary": 15, "major": 15}
WATERWAY_WIDTH = {"river": {14: 2, 15: 3, 16: 4, 17: 6}, "canal": {14: 2, 15: 3, 16: 4, 17: 5}}
PLACE_FROM = {"city": 14, "town": 14, "suburb": 14, "village": 14, "quarter": 15,
              "neighbourhood": 15, "hamlet": 16, "square": 16, "locality": 16}
BUILDINGS_FROM = 16
ICONS_FROM = 16
POI_NAMES_FROM = 17

# A label's pixel is ink from this much of full coverage, of 255.
INK_THRESHOLD = 88

INVERT = bytes(255 - i for i in range(256))

FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/Library/Fonts/DejaVuSans.ttf",
)


class RenderError(RuntimeError):
    pass


def find_font(explicit: str | None = None) -> str:
    if explicit:
        if not Path(explicit).is_file():
            raise RenderError(f"no font at {explicit}")
        return explicit
    for candidate in FONT_CANDIDATES:
        if Path(candidate).is_file():
            return candidate
    if shutil.which("fc-match"):
        found = subprocess.run(["fc-match", "-f", "%{file}", "DejaVu Sans"],
                               capture_output=True, text=True, check=False).stdout.strip()
        if found and Path(found).is_file():
            return found
    raise RenderError("DejaVu Sans not found: install it, or name a TTF with --font")


@dataclass
class Fonts:
    path: str

    def __post_init__(self):
        if not features.check("raqm"):
            raise RenderError("this Pillow has no libraqm, so it cannot shape Arabic: "
                              "install Pillow's wheel, or libraqm, fribidi and harfbuzz")
        self._cache: dict[int, ImageFont.FreeTypeFont] = {}

    def size(self, px: int) -> ImageFont.FreeTypeFont:
        if px not in self._cache:
            self._cache[px] = ImageFont.truetype(self.path, px,
                                                 layout_engine=ImageFont.Layout.RAQM)
        return self._cache[px]


# --------------------------------------------------------------------------
# Patterns, aligned to the world's pixels so they meet across tiles
# --------------------------------------------------------------------------

def _pattern(width: int, height: int, ox: int, oy: int, ink) -> Image.Image:
    """A mode-1 image, 255 where ink(gx, gy) holds for the world pixel."""
    rows = []
    cache: dict[int, bytes] = {}
    for r in range(height):
        gy = oy + r
        # Every pattern here repeats within 12 rows.
        key = gy % 12
        if key not in cache:
            bits = bytearray(width // 8)
            for c in range(width):
                if ink(ox + c, gy):
                    bits[c >> 3] |= 0x80 >> (c & 7)
            cache[key] = bytes(bits)
        rows.append(cache[key])
    return Image.frombytes("1", (width, height), b"".join(rows))


def water_ink(gx: int, gy: int) -> bool:
    return gy % 3 == 0 and gx % 2 == 0


def park_ink(gx: int, gy: int) -> bool:
    return (gx + gy) % 6 == 0


def building_ink(gx: int, gy: int) -> bool:
    return (gx + gy) % 2 == 0


# --------------------------------------------------------------------------
# Drawing a metatile
# --------------------------------------------------------------------------

class Metatile:
    def __init__(self, z: int, mx: int, my: int, fonts: Fonts):
        self.z = z
        self.size = META * TILE + 2 * MARGIN
        self.ox = mx * META * TILE - MARGIN
        self.oy = my * META * TILE - MARGIN
        self.img = Image.new("1", (self.size, self.size), 255)
        self.draw = ImageDraw.Draw(self.img)
        self.fonts = fonts
        self.taken: list[tuple[int, int, int, int]] = []
        self._patterns: dict[str, Image.Image] = {}

    # Projection ------------------------------------------------------------

    def px(self, point) -> tuple[float, float]:
        x, y = world_pixel(point[0], point[1], self.z)
        return x - self.ox, y - self.oy

    def line_px(self, line) -> list[tuple[float, float]]:
        return [self.px(p) for p in line]

    def meets(self, pts, pad: float = 0) -> bool:
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        return not (max(xs) < -pad or min(xs) > self.size + pad
                    or max(ys) < -pad or min(ys) > self.size + pad)

    # Areas -------------------------------------------------------------------

    def pattern(self, name: str, ink) -> Image.Image:
        if name not in self._patterns:
            self._patterns[name] = _pattern(self.size, self.size, self.ox, self.oy, ink)
        return self._patterns[name]

    def area(self, rings, ink, name: str) -> None:
        outer = self.line_px(rings[0])
        if len(outer) < 3 or not self.meets(outer):
            return
        mask = Image.new("1", self.img.size, 0)
        md = ImageDraw.Draw(mask)
        md.polygon(outer, fill=255)
        holes = [self.line_px(hole) for hole in rings[1:]]
        for hole in holes:
            md.polygon(hole, fill=0)
        self.img.paste(0, (0, 0), ImageChops.logical_and(mask, self.pattern(name, ink)))
        for ring in [outer, *holes]:
            self.draw.line(ring + [ring[0]], fill=0, width=1)

    # Lines -------------------------------------------------------------------

    def dashed(self, pts, on: int = 3, off: int = 3) -> None:
        carry = 0.0
        drawing = True
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            length = math.hypot(x2 - x1, y2 - y1)
            t = 0.0
            while t < length:
                step = min((on if drawing else off) - carry, length - t)
                if drawing:
                    a = (x1 + (x2 - x1) * t / length, y1 + (y2 - y1) * t / length)
                    b = (x1 + (x2 - x1) * (t + step) / length, y1 + (y2 - y1) * (t + step) / length)
                    self.draw.line([a, b], fill=0, width=1)
                t += step
                carry += step
                if carry >= (on if drawing else off):
                    carry = 0.0
                    drawing = not drawing

    def roads(self, data: MapData) -> None:
        visible = []
        for _id, cls, name, line in data.roads:
            style = ROAD_STYLE[cls].get(self.z)
            if style is None:
                continue
            pts = self.line_px(line)
            if self.meets(pts, pad=16):
                visible.append((cls, style, pts))
        order = {c: i for i, c in enumerate(ROAD_ORDER)}
        visible.sort(key=lambda v: order[v[0]])
        for cls, (outer, inner), pts in visible:
            if inner == "dash":
                self.dashed(pts)
            else:
                self.draw.line(pts, fill=0, width=outer, joint="curve" if outer > 2 else None)
        for cls, (outer, inner), pts in visible:
            if isinstance(inner, int) and inner > 0:
                self.draw.line(pts, fill=255, width=inner, joint="curve" if inner > 2 else None)

    def waterways(self, data: MapData) -> None:
        for _id, kind, line in data.waterways:
            width = WATERWAY_WIDTH.get(kind, {}).get(self.z, 1 if self.z >= 15 else 0)
            if not width:
                continue
            pts = self.line_px(line)
            if self.meets(pts, pad=8):
                self.draw.line(pts, fill=0, width=width, joint="curve" if width > 2 else None)

    # Labels and icons --------------------------------------------------------

    def free(self, box) -> bool:
        x0, y0, x1, y1 = box
        return all(x1 < a or x0 > c or y1 < b or y0 > d for a, b, c, d in self.taken)

    def label(self, text: str, at: tuple[float, float], size: int, angle: float = 0.0,
              halo: int = 2) -> bool:
        """Draws text centred on at, turned by angle degrees, with a white
        halo, if it meets no label already drawn. Whether it was drawn.

        The text is drawn anti-aliased and then cut at a third of full ink:
        FreeType's own one-bit mode drops the thin joins of Arabic letters
        and the hairlines of Latin ones, where this keeps them."""
        font = self.fonts.size(size)
        probe = ImageDraw.Draw(Image.new("L", (1, 1)))
        l, t, r, b = probe.textbbox((0, 0), text, font=font)
        pad = halo + 1
        w, h = r - l + 2 * pad, b - t + 2 * pad
        text_img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(text_img).text((pad - l, pad - t), text, font=font, fill=255)
        if angle:
            text_img = text_img.rotate(angle, resample=Image.BILINEAR, expand=True)
        glyphs = text_img.point(lambda v: 255 if v >= INK_THRESHOLD else 0, "1")
        around = glyphs.convert("L").filter(ImageFilter.MaxFilter(2 * halo + 1)).point(
            lambda v: 255 if v else 0, "1")
        w, h = glyphs.size
        x0 = int(round(at[0] - w / 2))
        y0 = int(round(at[1] - h / 2))
        box = (x0, y0, x0 + w, y0 + h)
        if x0 + w < 0 or y0 + h < 0 or x0 > self.size or y0 > self.size or not self.free(box):
            return False
        self.taken.append(box)
        self.img.paste(255, (x0, y0), around)
        self.img.paste(0, (x0, y0), glyphs)
        return True

    def icon(self, kind: str, at: tuple[float, float]) -> bool:
        rows = icons.ICONS.get(kind)
        if rows is None:
            return False
        half = icons.SIZE // 2 + 1
        x0, y0 = int(round(at[0])) - half, int(round(at[1])) - half
        box = (x0, y0, x0 + 2 * half, y0 + 2 * half)
        if not self.free(box):
            return False
        self.taken.append(box)
        self.draw.rectangle(box, fill=255, outline=0)
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch == "#":
                    self.img.putpixel((x0 + 1 + i, y0 + 1 + j), 0)
        return True

    def labels(self, data: MapData) -> None:
        for _id, kind, name, point in data.places:
            if self.z >= PLACE_FROM.get(kind, 99):
                self.label(name, self.px(point), 16 if self.z >= 16 else 15)
        if self.z >= ICONS_FROM:
            for _id, kind, name, point in data.pois:
                at = self.px(point)
                if self.icon(kind, at) and name and self.z >= POI_NAMES_FROM:
                    self.label(name, (at[0], at[1] + icons.SIZE + 2), 11)
        for _id, cls, name, line in data.roads:
            if not name or self.z < ROAD_LABEL_FROM.get(cls, 99):
                continue
            pts = self.line_px(line)
            if not self.meets(pts):
                continue
            at, angle, room = _midpoint(pts)
            font = self.fonts.size(12)
            width = ImageDraw.Draw(Image.new("L", (1, 1))).textlength(name, font=font)
            if room >= width + 16:
                self.label(name, at, 12, angle)

    def render(self, data: MapData) -> None:
        for _id, rings in data.water:
            self.area(rings, water_ink, "water")
        for _id, rings in data.parks:
            self.area(rings, park_ink, "park")
        if self.z >= BUILDINGS_FROM:
            for _id, rings in data.buildings:
                self.area(rings, building_ink, "building")
        self.waterways(data)
        self.roads(data)
        self.labels(data)

    def tile(self, i: int, j: int) -> bytes:
        """Tile (i, j) of this block: 8192 bytes, rows top down, the left
        pixel the high bit, 1 for ink."""
        left, top = MARGIN + i * TILE, MARGIN + j * TILE
        raw = self.img.crop((left, top, left + TILE, top + TILE)).tobytes()
        return raw.translate(INVERT)


def _midpoint(pts):
    """The point halfway along a line, the angle of the segment there (for
    upright text, -90 to 90 degrees, counter-clockwise), and the length of
    that segment."""
    lengths = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:])]
    half = sum(lengths) / 2
    for (a, b), length in zip(zip(pts, pts[1:]), lengths):
        if half <= length and length > 0:
            t = half / length
            at = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
            angle = -math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
            if angle > 90:
                angle -= 180
            elif angle <= -90:
                angle += 180
            return at, angle, length
        half -= length
    return pts[-1], 0.0, 0.0


def render_tiles(data: MapData, tiles: list[tuple[int, int, int]], fonts: Fonts,
                 progress=None) -> dict[tuple[int, int, int], bytes]:
    """Every tile asked for, drawn: (z, x, y) to its bitmap."""
    blocks: dict[tuple[int, int, int], list[tuple[int, int]]] = {}
    for z, x, y in tiles:
        blocks.setdefault((z, x // META, y // META), []).append((x, y))
    out: dict[tuple[int, int, int], bytes] = {}
    for n, ((z, mx, my), members) in enumerate(sorted(blocks.items())):
        block = Metatile(z, mx, my, fonts)
        block.render(data)
        for x, y in members:
            out[(z, x, y)] = block.tile(x - mx * META, y - my * META)
        if progress:
            progress(n + 1, len(blocks))
    return out
