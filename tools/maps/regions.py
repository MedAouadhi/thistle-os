# SPDX-License-Identifier: BSD-3-Clause
"""Regions: the places a Map Pack covers, as the user drew them, and which
tiles each one needs.

A Regions file is GeoJSON (docs/maps/map-pack-format.md has the details):
a FeatureCollection whose features are circles, a Point with
properties.radius_m, or freehand polygons, a Polygon's outer ring. Each may
set properties.name and properties.deepest_zoom, 14 to 17, default 16.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path

MIN_ZOOM = 14
MAX_ZOOM = 17
DEFAULT_DEEPEST = 16
EARTH_RADIUS_M = 6_371_008.8


class RegionsError(ValueError):
    """A Regions file the tool cannot build from; the message says why."""


@dataclass(frozen=True)
class Region:
    name: str
    deepest_zoom: int
    # A circle: centre (lat, lon) and radius. A polygon: its ring of (lat,
    # lon) points, not closed.
    centre: tuple[float, float] | None = None
    radius_m: float = 0.0
    points: tuple[tuple[float, float], ...] = field(default_factory=tuple)

    @property
    def is_circle(self) -> bool:
        return self.centre is not None

    def bbox(self) -> tuple[float, float, float, float]:
        """(south, west, north, east)."""
        if self.is_circle:
            lat, lon = self.centre
            dlat = math.degrees(self.radius_m / EARTH_RADIUS_M)
            dlon = dlat / max(math.cos(math.radians(lat)), 1e-6)
            return lat - dlat, lon - dlon, lat + dlat, lon + dlon
        lats = [p[0] for p in self.points]
        lons = [p[1] for p in self.points]
        return min(lats), min(lons), max(lats), max(lons)

    def intersects(self, south: float, west: float, north: float, east: float) -> bool:
        """Whether any of the box (south, west, north, east) is inside."""
        if self.is_circle:
            lat, lon = self.centre
            nearest = (min(max(lat, south), north), min(max(lon, west), east))
            return distance_m(self.centre, nearest) <= self.radius_m
        return _polygon_meets_box(self.points, south, west, north, east)


def distance_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    (la1, lo1), (la2, lo2) = a, b
    p1, p2 = math.radians(la1), math.radians(la2)
    h = (math.sin((p2 - p1) / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lo2 - lo1) / 2) ** 2)
    return 2 * EARTH_RADIUS_M * math.asin(min(1.0, math.sqrt(h)))


def _point_in_ring(lat: float, lon: float, ring) -> bool:
    inside = False
    n = len(ring)
    for i in range(n):
        (y1, x1), (y2, x2) = ring[i], ring[(i + 1) % n]
        if (y1 > lat) != (y2 > lat):
            x = x1 + (lat - y1) * (x2 - x1) / (y2 - y1)
            if lon < x:
                inside = not inside
    return inside


def _segments_cross(p1, p2, q1, q2) -> bool:
    def orient(a, b, c):
        v = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        return (v > 0) - (v < 0)

    def on(a, b, c):
        return (min(a[0], b[0]) <= c[0] <= max(a[0], b[0])
                and min(a[1], b[1]) <= c[1] <= max(a[1], b[1]))

    o1, o2, o3, o4 = orient(p1, p2, q1), orient(p1, p2, q2), orient(q1, q2, p1), orient(q1, q2, p2)
    if o1 != o2 and o3 != o4:
        return True
    return ((o1 == 0 and on(p1, p2, q1)) or (o2 == 0 and on(p1, p2, q2))
            or (o3 == 0 and on(q1, q2, p1)) or (o4 == 0 and on(q1, q2, p2)))


def _polygon_meets_box(ring, south, west, north, east) -> bool:
    if any(south <= lat <= north and west <= lon <= east for lat, lon in ring):
        return True
    corners = [(south, west), (south, east), (north, east), (north, west)]
    if any(_point_in_ring(lat, lon, ring) for lat, lon in corners):
        return True
    n = len(ring)
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        for k in range(4):
            if _segments_cross(a, b, corners[k], corners[(k + 1) % 4]):
                return True
    return False


# --------------------------------------------------------------------------
# Web Mercator tiles
# --------------------------------------------------------------------------

def world_pixel(lat: float, lon: float, z: int) -> tuple[float, float]:
    size = 256 * (1 << z)
    x = (lon + 180.0) / 360.0 * size
    s = math.sin(math.radians(max(min(lat, 85.05112878), -85.05112878)))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * size
    return x, y


def tile_bounds(z: int, x: int, y: int) -> tuple[float, float, float, float]:
    """(south, west, north, east) of a tile."""
    n = 1 << z

    def lat(ty: int) -> float:
        return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * ty / n))))

    return lat(y + 1), x / n * 360.0 - 180.0, lat(y), (x + 1) / n * 360.0 - 180.0


def _tile_range(points, z: int) -> tuple[int, int, int, int]:
    """x0, y0, x1, y1 of the tiles around points, one more each way, as a
    point on a tile's edge is in both tiles."""
    xs, ys = zip(*(world_pixel(lat, lon, z) for lat, lon in points))
    return (int(min(xs) // 256) - 1, int(min(ys) // 256) - 1,
            int(max(xs) // 256) + 1, int(max(ys) // 256) + 1)


def _segment_meets_box(a, b, south, west, north, east) -> bool:
    if any(south <= p[0] <= north and west <= p[1] <= east for p in (a, b)):
        return True
    corners = [(south, west), (south, east), (north, east), (north, west)]
    return any(_segments_cross(a, b, corners[k], corners[(k + 1) % 4]) for k in range(4))


def _polygon_tiles(ring, z: int) -> set[tuple[int, int]]:
    """What _polygon_meets_box gives for every tile, quick enough for the
    browser mode's estimate of a freehand Region tens of kilometres across.

    A tile the outline passes through is the polygon's; any other tile is
    wholly inside or wholly outside, so its centre says which. The first
    are found edge by edge, the second row by row along a line through the
    row's centres."""
    out: set[tuple[int, int]] = set()
    n = len(ring)
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        x0, y0, x1, y1 = _tile_range((a, b), z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                if (x, y) not in out and _segment_meets_box(a, b, *tile_bounds(z, x, y)):
                    out.add((x, y))
    size = 1 << z
    x0, y0, x1, y1 = _tile_range(ring, z)
    for y in range(max(y0, 0), min(y1, size - 1) + 1):
        south, _w, north, _e = tile_bounds(z, 0, y)
        lat = (south + north) / 2
        crossings = sorted(
            y1_lon + (lat - y1_lat) * (y2_lon - y1_lon) / (y2_lat - y1_lat)
            for (y1_lat, y1_lon), (y2_lat, y2_lon) in zip(ring, ring[1:] + ring[:1])
            if (y1_lat > lat) != (y2_lat > lat))
        for left, right in zip(crossings[0::2], crossings[1::2]):
            first = max(int((left + 180.0) / 360.0 * size), x0)
            for x in range(first, min(int((right + 180.0) / 360.0 * size), x1) + 1):
                centre = (x + 0.5) / size * 360.0 - 180.0
                if left < centre < right:
                    out.add((x, y))
    return out


def region_tiles(region: Region, z: int) -> set[tuple[int, int]]:
    """The (x, y) of every tile at z that the Region touches."""
    if not region.is_circle:
        return _polygon_tiles(list(region.points), z)
    s, w, n, e = region.bbox()
    x0, y0 = (int(v // 256) for v in world_pixel(n, w, z))
    x1, y1 = (int(v // 256) for v in world_pixel(s, e, z))
    return {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)
            if region.intersects(*tile_bounds(z, x, y))}


def pack_tiles(regions: list[Region]) -> list[tuple[int, int, int]]:
    """Every (z, x, y) a Pack of these Regions holds, sorted: each Region's
    tiles from z14 to its own deepest level."""
    tiles: set[tuple[int, int, int]] = set()
    for region in regions:
        for z in range(MIN_ZOOM, region.deepest_zoom + 1):
            tiles.update((z, x, y) for x, y in region_tiles(region, z))
    return sorted(tiles)


# --------------------------------------------------------------------------
# The file
# --------------------------------------------------------------------------

def parse_regions(document: dict) -> list[Region]:
    if not isinstance(document, dict) or document.get("type") != "FeatureCollection":
        raise RegionsError("a Regions file is a GeoJSON FeatureCollection")
    regions = []
    for i, feature in enumerate(document.get("features") or []):
        props = feature.get("properties") or {}
        geometry = feature.get("geometry") or {}
        name = str(props.get("name") or f"Region {i + 1}")
        deepest = props.get("deepest_zoom", DEFAULT_DEEPEST)
        if not isinstance(deepest, int) or not MIN_ZOOM <= deepest <= MAX_ZOOM:
            raise RegionsError(f"{name}: deepest_zoom must be {MIN_ZOOM} to {MAX_ZOOM}, not {deepest!r}")
        kind = geometry.get("type")
        if kind == "Point":
            lon, lat = (float(v) for v in geometry["coordinates"][:2])
            radius = props.get("radius_m")
            if not isinstance(radius, (int, float)) or radius <= 0:
                raise RegionsError(f"{name}: a circle needs a positive radius_m")
            regions.append(Region(name, deepest, centre=(lat, lon), radius_m=float(radius)))
        elif kind == "Polygon":
            ring = [(float(lat), float(lon)) for lon, lat, *_ in geometry["coordinates"][0]]
            if len(ring) > 1 and ring[0] == ring[-1]:
                ring.pop()
            if len(ring) < 3:
                raise RegionsError(f"{name}: a polygon needs at least three points")
            regions.append(Region(name, deepest, points=tuple(ring)))
        else:
            raise RegionsError(f"{name}: a Region is a circle (Point and radius_m) or a Polygon, not {kind!r}")
    if not regions:
        raise RegionsError("the Regions file has no Regions")
    return regions


def load_regions(path: Path | str) -> list[Region]:
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RegionsError(f"cannot read {path}: {exc}") from exc
    return parse_regions(document)
