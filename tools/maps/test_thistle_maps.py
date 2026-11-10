#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The Map Pack tool's tests, at its one seam: a Regions file and an extract
go in, a Pack comes out, and the Pack is read back here by a reader written
from docs/maps/map-pack-format.md alone, sharing no code with the tool.

    python3 tools/maps/test_thistle_maps.py

The golden Pack, testdata/fixture.tmap, is the fixture built by the tool.
After a deliberate change to the format or the cartography, rebuild it:

    python3 tools/maps/thistle_maps.py build tools/maps/testdata/fixture-regions.geojson \\
        --extract tools/maps/testdata/fixture.osm -o tools/maps/testdata/fixture.tmap
"""

from __future__ import annotations

import functools
import http.server
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE / "thistle_maps.py"
DATA = HERE / "testdata"
REGIONS = DATA / "fixture-regions.geojson"
EXTRACT = DATA / "fixture.osm"
GOLDEN = DATA / "fixture.tmap"

MEDINA = (36.8000, 10.1800)
LAC_NORD = (36.8150, 10.2000)
QUARTER = "محمد"


# --------------------------------------------------------------------------
# An independent reader, from the format document
# --------------------------------------------------------------------------

class Pack:
    """A Map Pack, read and checked strictly. Raises ValueError on anything
    the format does not allow."""

    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 32 or data[:8] != b"THSTMAP\x00":
            raise ValueError("not a Map Pack")
        (self.major, self.minor, header_len, self.variant, _r0,
         self.min_zoom, self.max_zoom, section_count, self.tile_count,
         _r1, header_crc) = struct.unpack_from("<HHHBBBBHIII", data, 8)
        if header_len != 32:
            raise ValueError("header length")
        directory = data[32:32 + 16 * section_count]
        if len(directory) != 16 * section_count:
            raise ValueError("directory cut short")
        if zlib.crc32(data[:28] + directory) != header_crc:
            raise ValueError("header CRC")
        self.sections: dict[bytes, bytes] = {}
        for i in range(section_count):
            tag, offset, length, crc = struct.unpack_from("<4sIII", directory, 16 * i)
            body = data[offset:offset + length]
            if len(body) != length:
                raise ValueError(f"section {tag!r} out of the file")
            # TILE's CRC is 0: each tile has its own.
            if (0 if tag == b"TILE" else zlib.crc32(body)) != crc:
                raise ValueError(f"section {tag!r} CRC")
            self.sections[tag] = body

    @property
    def credit(self) -> str:
        return self.sections[b"ATTR"].decode("utf-8")

    def regions(self) -> list[dict]:
        body = self.sections[b"REGN"]
        (count,) = struct.unpack_from("<H", body, 0)
        at = 2
        out = []
        for _ in range(count):
            kind, deepest, name_len = struct.unpack_from("<BBH", body, at)
            at += 4
            name = body[at:at + name_len].decode("utf-8")
            at += name_len
            region = {"name": name, "deepest_zoom": deepest}
            if kind == 0:
                lat, lon, radius = struct.unpack_from("<iiI", body, at)
                at += 12
                region.update(kind="circle", centre=(lat / 1e7, lon / 1e7), radius_m=radius)
            elif kind == 1:
                (n,) = struct.unpack_from("<H", body, at)
                at += 2
                pts = [struct.unpack_from("<ii", body, at + 8 * k) for k in range(n)]
                at += 8 * n
                region.update(kind="polygon", points=[(a / 1e7, b / 1e7) for a, b in pts])
            else:
                raise ValueError(f"region kind {kind}")
            out.append(region)
        if at != len(body):
            raise ValueError("trailing bytes in REGN")
        return out

    def index(self) -> dict[tuple[int, int, int], tuple[int, int, int]]:
        body = self.sections[b"TIDX"]
        if len(body) != 24 * self.tile_count:
            raise ValueError("index length")
        out = {}
        previous = None
        for i in range(self.tile_count):
            x, y, z, _flags, _r, offset, length, crc = struct.unpack_from("<IIBBHIII", body, 24 * i)
            key = (z, x, y)
            if previous is not None and key <= previous:
                raise ValueError("index not sorted by z, x, y")
            previous = key
            out[key] = (offset, length, crc)
        return out

    def tile(self, z: int, x: int, y: int) -> bytes:
        offset, length, crc = self.index()[(z, x, y)]
        payload = self.data[offset:offset + length]
        if len(payload) != length:
            raise ValueError("payload out of the file")
        bitmap = zlib.decompress(payload, wbits=-15)
        if len(bitmap) != 8192:
            raise ValueError("tile not 256 x 256 at one bit")
        if zlib.crc32(bitmap) != crc:
            raise ValueError("tile CRC")
        return bitmap


def ink(bitmap: bytes, px: int, py: int) -> bool:
    return bool(bitmap[py * 32 + px // 8] & (0x80 >> (px % 8)))


def world_pixel(lat: float, lon: float, z: int) -> tuple[float, float]:
    """Web Mercator, 256 px tiles."""
    size = 256 * (1 << z)
    x = (lon + 180.0) / 360.0 * size
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * size
    return x, y


def tile_bounds(z: int, x: int, y: int) -> tuple[float, float, float, float]:
    """(south, west, north, east) of a tile."""
    n = 1 << z

    def lat(ty: int) -> float:
        return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * ty / n))))

    return lat(y + 1), x / n * 360 - 180, lat(y), (x + 1) / n * 360 - 180


def metres(a: tuple[float, float], b: tuple[float, float]) -> float:
    (la1, lo1), (la2, lo2) = a, b
    p1, p2 = math.radians(la1), math.radians(la2)
    h = (math.sin((p2 - p1) / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lo2 - lo1) / 2) ** 2)
    return 2 * 6_371_008.8 * math.asin(math.sqrt(h))


def components(points: set[tuple[int, int]]) -> list[set[tuple[int, int]]]:
    """8-connected groups of inked pixels."""
    left = set(points)
    out = []
    while left:
        start = left.pop()
        group = {start}
        todo = [start]
        while todo:
            px, py = todo.pop()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (px + dx, py + dy)
                    if q in left:
                        left.remove(q)
                        group.add(q)
                        todo.append(q)
        out.append(group)
    return out


def polygon_tiles(ring: list[tuple[float, float]], z: int) -> set[tuple[int, int]]:
    """The format document's rule, by brute force over the ring's box: a
    tile is the polygon's if they overlap, edges and corners included, in
    latitude and longitude. ring is (lat, lon), not closed."""

    def inside(lat, lon):
        hit = False
        for (a, b), (c, d) in zip(ring, ring[1:] + ring[:1]):
            if (a > lat) != (c > lat) and lon < b + (lat - a) * (d - b) / (c - a):
                hit = not hit
        return hit

    def cross(p, q, r, s):
        def side(a, b, c):
            v = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            return (v > 0) - (v < 0)

        def within(a, b, c):
            return min(a[0], b[0]) <= c[0] <= max(a[0], b[0]) and min(a[1], b[1]) <= c[1] <= max(a[1], b[1])

        d1, d2, d3, d4 = side(p, q, r), side(p, q, s), side(r, s, p), side(r, s, q)
        if d1 != d2 and d3 != d4:
            return True
        return any(d == 0 and within(a, b, c) for d, a, b, c in
                   ((d1, p, q, r), (d2, p, q, s), (d3, r, s, p), (d4, r, s, q)))

    lats, lons = [p[0] for p in ring], [p[1] for p in ring]
    x0, y0 = (int(v // 256) for v in world_pixel(max(lats), min(lons), z))
    x1, y1 = (int(v // 256) for v in world_pixel(min(lats), max(lons), z))
    out = set()
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            s, w, n, e = tile_bounds(z, x, y)
            corners = [(s, w), (s, e), (n, e), (n, w)]
            if (any(s <= a <= n and w <= b <= e for a, b in ring)
                    or any(inside(a, b) for a, b in corners)
                    or any(cross(p, q, corners[k], corners[(k + 1) % 4])
                           for p, q in zip(ring, ring[1:] + ring[:1]) for k in range(4))):
                out.add((x, y))
    return out


def freehand(centre: tuple[float, float], radius_m: float, points: int) -> list[tuple[float, float]]:
    """A wobbly, concave ring as a hand draws one: a C open to the east."""
    lat0, lon0 = centre
    ring = []
    for i in range(points):
        a = 2 * math.pi * i / points
        r = radius_m * (1 + 0.15 * math.sin(7 * a)) * (0.35 if abs(a - math.pi * 2) < 0.6 or a < 0.6 else 1)
        ring.append((lat0 + r * math.sin(a) / 111_320.0,
                     lon0 + r * math.cos(a) / (111_320.0 * math.cos(math.radians(lat0)))))
    return ring


def regions_file(path: Path, features: list[dict]) -> Path:
    path.write_text(json.dumps({"type": "FeatureCollection", "thistle_regions": 1, "features": features}))
    return path


def polygon_feature(ring, deepest: int, name: str = "Freehand") -> dict:
    coords = [[lon, lat] for lat, lon in ring] + [[ring[0][1], ring[0][0]]]
    return {"type": "Feature", "properties": {"name": name, "deepest_zoom": deepest},
            "geometry": {"type": "Polygon", "coordinates": [coords]}}


# --------------------------------------------------------------------------
# Running the tool
# --------------------------------------------------------------------------

def run_tool(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    run = subprocess.run([sys.executable, str(TOOL), *args], text=True,
                         capture_output=True, check=False, timeout=600)
    if check and run.returncode != 0:
        raise AssertionError(f"tool failed ({run.returncode}):\n{run.stdout}\n{run.stderr}")
    return run


@functools.lru_cache(maxsize=None)
def fresh_build() -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "fixture.tmap"
        run_tool("build", str(REGIONS), "--extract", str(EXTRACT), "-o", str(out))
        return out.read_bytes()


class GoldenPack(unittest.TestCase):
    """What the checked-in Pack holds, read by the reader above."""

    @classmethod
    def setUpClass(cls):
        cls.pack = Pack(GOLDEN.read_bytes())

    def test_version_and_variant(self):
        self.assertEqual((self.pack.major, self.pack.minor), (1, 0))
        self.assertEqual(self.pack.variant, 0)  # one bit, black on white
        self.assertEqual((self.pack.min_zoom, self.pack.max_zoom), (14, 17))

    def test_credit_is_openstreetmap(self):
        self.assertIn("© OpenStreetMap contributors", self.pack.credit)
        self.assertIn("ODbL", self.pack.credit)

    def test_both_regions_with_their_own_depth(self):
        regions = self.pack.regions()
        self.assertEqual([r["name"] for r in regions], ["Medina", "Lac Nord"])
        medina, lac = regions
        self.assertEqual((medina["kind"], medina["deepest_zoom"], medina["radius_m"]),
                         ("circle", 17, 350))
        self.assertAlmostEqual(medina["centre"][0], MEDINA[0], places=6)
        self.assertAlmostEqual(medina["centre"][1], MEDINA[1], places=6)
        self.assertEqual((lac["kind"], lac["deepest_zoom"]), ("polygon", 15))
        self.assertEqual(len(lac["points"]), 6)  # the ring, not closed twice

    def test_every_tile_inflates_to_its_crc(self):
        index = self.pack.index()
        self.assertEqual(len(index), self.pack.tile_count)
        for z, x, y in index:
            self.pack.tile(z, x, y)

    def test_each_region_is_covered_to_its_depth_and_no_deeper(self):
        index = self.pack.index()
        for z in range(14, 18):
            mx, my = world_pixel(*MEDINA, z)
            self.assertIn((z, int(mx // 256), int(my // 256)), index, f"Medina z{z}")
        for z in (14, 15):
            lx, ly = world_pixel(*LAC_NORD, z)
            self.assertIn((z, int(lx // 256), int(ly // 256)), index, f"Lac Nord z{z}")
        # Deeper than z15, only the Medina's circle has tiles.
        for (z, x, y) in index:
            if z >= 16:
                s, w, n, e = tile_bounds(z, x, y)
                nearest = (min(max(MEDINA[0], s), n), min(max(MEDINA[1], w), e))
                self.assertLessEqual(metres(MEDINA, nearest), 351, f"z{z} {x},{y}")

    def test_no_tile_is_far_from_every_region(self):
        for (z, x, y) in self.pack.index():
            s, w, n, e = tile_bounds(z, x, y)
            centre = ((s + n) / 2, (w + e) / 2)
            diagonal = metres((s, w), (n, e))
            near = min(metres(centre, MEDINA) - 350, metres(centre, LAC_NORD) - 360)
            self.assertLess(near, diagonal / 2 + 1, f"z{z} {x},{y}")

    def test_the_quarter_name_is_drawn_joined(self):
        """محمد has no dots: shaped, its four letters are one connected
        shape; drawn letter by letter it would be four."""
        lat, lon = 36.8000 + 120 / 111_320.0, 10.1800 + 120 / (111_320.0 * math.cos(math.radians(36.8)))
        for z in (16, 17):
            gx, gy = world_pixel(lat, lon, z)
            inked = set()
            for py in range(int(gy) - 14, int(gy) + 15):
                for px in range(int(gx) - 40, int(gx) + 41):
                    tile = self.pack.tile(z, px // 256, py // 256)
                    if ink(tile, px % 256, py % 256):
                        inked.add((px, py))
            groups = [g for g in components(inked) if len(g) > 3]
            self.assertEqual(len(groups), 1, f"z{z}: {len(groups)} shapes")
            width = max(p[0] for p in groups[0]) - min(p[0] for p in groups[0])
            self.assertGreater(width, 15, f"z{z}")

    def test_buildings_only_at_the_deepest_two_levels(self):
        """Five houses in a row south of the avenue: ink at z16 and z17,
        none where they stand at z15."""
        lat = 36.8000 - 130 / 111_320.0
        lons = [10.1800 + (-45 + 45 * i) / (111_320.0 * math.cos(math.radians(36.8))) for i in range(5)]
        for z, expect in ((15, False), (16, True), (17, True)):
            found = False
            for lon in lons:
                gx, gy = world_pixel(lat, lon, z)
                tile = self.pack.tile(z, int(gx) // 256, int(gy) // 256)
                if any(ink(tile, (int(gx) + dx) % 256, (int(gy) + dy) % 256)
                       for dx in (-2, -1, 0, 1, 2) for dy in (-2, -1, 0, 1, 2)
                       if int(gx) // 256 == (int(gx) + dx) // 256
                       and int(gy) // 256 == (int(gy) + dy) // 256):
                    found = True
            self.assertEqual(found, expect, f"z{z}")


class Building(unittest.TestCase):
    """The tool, run now."""

    def test_builds_the_same_pack_on_every_run(self):
        first = fresh_build()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "again.tmap"
            run_tool("build", str(REGIONS), "--extract", str(EXTRACT), "-o", str(out))
            self.assertEqual(out.read_bytes(), first)

    def test_the_golden_pack_is_what_the_tool_builds(self):
        """The same tiles, Regions and credit. The pixels are not compared:
        another FreeType may place a label a pixel over."""
        fresh, golden = Pack(fresh_build()), Pack(GOLDEN.read_bytes())
        self.assertEqual(sorted(fresh.index()), sorted(golden.index()))
        self.assertEqual(fresh.regions(), golden.regions())
        self.assertEqual(fresh.credit, golden.credit)

    def test_estimate_counts_the_tiles_a_build_writes(self):
        run = run_tool("estimate", str(REGIONS), "--json")
        estimate = json.loads(run.stdout)
        self.assertEqual(estimate["tiles"], Pack(fresh_build()).tile_count)
        self.assertGreater(estimate["bytes"], 0)
        self.assertEqual(sum(estimate["tiles_per_zoom"].values()), estimate["tiles"])

    def test_estimate_is_importable_for_the_browser_mode(self):
        sys.path.insert(0, str(HERE))
        try:
            import estimate
            from regions import load_regions
        finally:
            sys.path.remove(str(HERE))
        result = estimate.estimate_pack(load_regions(REGIONS))
        self.assertEqual(result.tiles, Pack(fresh_build()).tile_count)

    def test_a_freehand_polygon_needs_the_tiles_the_format_says(self):
        """A concave ring of 160 points, as the browser mode saves one:
        every level's tiles are those the format's rule gives."""
        ring = freehand((36.80, 10.18), 1800, 160)
        with tempfile.TemporaryDirectory() as tmp:
            path = regions_file(Path(tmp) / "c.geojson", [polygon_feature(ring, 17)])
            estimate = json.loads(run_tool("estimate", str(path), "--json").stdout)
        for z in range(14, 18):
            self.assertEqual(estimate["tiles_per_zoom"][str(z)], len(polygon_tiles(ring, z)), f"z{z}")

    def test_the_estimate_of_a_big_freehand_polygon_is_quick(self):
        """The browser mode estimates on every stroke: a 60 km polygon at
        z17, about 50 000 tiles, takes a few seconds at most."""
        ring = freehand((52.52, 13.40), 30_000, 300)
        with tempfile.TemporaryDirectory() as tmp:
            path = regions_file(Path(tmp) / "big.geojson", [polygon_feature(ring, 17)])
            started = time.monotonic()
            estimate = json.loads(run_tool("estimate", str(path), "--json").stdout)
            took = time.monotonic() - started
        self.assertGreater(estimate["tiles_per_zoom"]["17"], 40_000)
        self.assertLess(took, 4.0)

    def test_a_place_icon_on_the_edge_of_a_drawn_block_is_cut_not_a_crash(self):
        """The tool draws 8 x 8 tiles at a time with a margin around them;
        an icon straddling the margin's edge was drawn off the image, and
        the build died with 'image index out of range'."""
        z, block = 16, 8 * 256  # icons are drawn from z16
        # 1 px inside the right edge of block 4 (and well inside block 5)
        lon = (5 * block + 96 - 1) / (256 * (1 << z)) * 360.0 - 180.0
        lat = 36.8
        extract = ('<?xml version="1.0" encoding="UTF-8"?>\n<osm version="0.6">\n'
                   f'  <node id="1" version="1" lat="{lat}" lon="{lon:.7f}">\n'
                   '    <tag k="amenity" v="pharmacy"/>\n  </node>\n</osm>\n')
        regions = {"type": "FeatureCollection", "features": [
            {"type": "Feature", "properties": {"radius_m": 300, "deepest_zoom": 16},
             "geometry": {"type": "Point", "coordinates": [lon, lat]}}]}
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "edge.osm").write_text(extract)
            (Path(tmp) / "edge.geojson").write_text(json.dumps(regions))
            out = Path(tmp) / "edge.tmap"
            run_tool("build", str(Path(tmp) / "edge.geojson"), "--extract",
                     str(Path(tmp) / "edge.osm"), "-o", str(out))
            pack = Pack(out.read_bytes())
            self.assertGreaterEqual(len({x // 8 for zz, x, _y in pack.index() if zz == z}), 2)
            for key in pack.index():
                pack.tile(*key)

    def test_a_regions_file_with_no_regions_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "empty.geojson"
            empty.write_text('{"type": "FeatureCollection", "features": []}')
            run = run_tool("build", str(empty), "--extract", str(EXTRACT),
                           "-o", str(Path(tmp) / "x.tmap"), check=False)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("no Regions", run.stderr)

    def test_a_depth_outside_the_four_levels_is_refused(self):
        regions = json.loads(REGIONS.read_text())
        regions["features"][0]["properties"]["deepest_zoom"] = 18
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "deep.geojson"
            path.write_text(json.dumps(regions))
            run = run_tool("build", str(path), "--extract", str(EXTRACT),
                           "-o", str(Path(tmp) / "x.tmap"), check=False)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("deepest_zoom", run.stderr)


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class Fetching(unittest.TestCase):
    """The extract comes from Geofabrik, picked by the Regions, fetched once
    and then read from the cache with no network."""

    def test_fetches_the_smallest_extract_once_then_builds_offline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            site = root / "site"
            site.mkdir()
            shutil.copy(EXTRACT, site / "tunis.osm")
            (site / "unrelated.osm").write_text("not an extract")

            def square(w, s, e, n):
                return {"type": "MultiPolygon", "coordinates": [[[[w, s], [e, s], [e, n], [w, n], [w, s]]]]}

            server = http.server.ThreadingHTTPServer(
                ("127.0.0.1", 0), functools.partial(_Quiet, directory=str(site)))
            base = f"http://127.0.0.1:{server.server_address[1]}"
            index = {"type": "FeatureCollection", "features": [
                {"type": "Feature", "geometry": square(7, 30, 12, 38),
                 "properties": {"id": "tunisia", "urls": {"pbf": f"{base}/missing.osm"}}},
                {"type": "Feature", "geometry": square(10, 36.5, 10.5, 37),
                 "properties": {"id": "tunis", "urls": {"pbf": f"{base}/tunis.osm"}}},
                {"type": "Feature", "geometry": square(20, 50, 21, 51),
                 "properties": {"id": "elsewhere", "urls": {"pbf": f"{base}/unrelated.osm"}}},
            ]}
            (site / "index-v1.json").write_text(json.dumps(index))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            cache = root / "cache"
            try:
                run = run_tool("build", str(REGIONS), "--cache", str(cache),
                               "--geofabrik-index", f"{base}/index-v1.json",
                               "-o", str(root / "first.tmap"))
            finally:
                server.shutdown()
                server.server_close()
            self.assertIn("extract tunis", run.stdout)
            self.assertEqual(Pack((root / "first.tmap").read_bytes()).tile_count,
                             Pack(fresh_build()).tile_count)

            # The server is gone: the cache alone builds the same Pack.
            run_tool("build", str(REGIONS), "--cache", str(cache),
                     "--geofabrik-index", f"{base}/index-v1.json",
                     "-o", str(root / "second.tmap"))
            self.assertEqual((root / "second.tmap").read_bytes(),
                             (root / "first.tmap").read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
