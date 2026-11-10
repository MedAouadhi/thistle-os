# SPDX-License-Identifier: BSD-3-Clause
"""Where the map's data comes from: a Geofabrik extract, picked for the
Regions, fetched once into a cache, and read clipped to what the Regions
need.

Geofabrik's index (https://download.geofabrik.de/index-v1.json) lists every
extract with its outline. The tool takes, for each Region, the smallest
extract whose outline holds the whole Region, and reads each extract once.
A cached extract is used as it is, with no network, so a rebuild works
offline; --refresh fetches it again.
"""

from __future__ import annotations

import json
import math
import os
import shutil
import tempfile
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

import osmium

from regions import Region

GEOFABRIK_INDEX = "https://download.geofabrik.de/index-v1.json"
USER_AGENT = "thistle-maps (ThistleOS Map Pack tool)"


def default_cache() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "thistle-maps"


# --------------------------------------------------------------------------
# Picking and fetching
# --------------------------------------------------------------------------

def _in_ring(lon: float, lat: float, ring) -> bool:
    inside = False
    n = len(ring)
    for i in range(n):
        x1, y1 = ring[i][:2]
        x2, y2 = ring[(i + 1) % n][:2]
        if (y1 > lat) != (y2 > lat):
            if lon < x1 + (lat - y1) * (x2 - x1) / (y2 - y1):
                inside = not inside
    return inside


def _in_multipolygon(lon: float, lat: float, geometry: dict) -> bool:
    polygons = geometry["coordinates"] if geometry.get("type") == "MultiPolygon" else [geometry["coordinates"]]
    for polygon in polygons:
        if polygon and _in_ring(lon, lat, polygon[0]) and not any(
                _in_ring(lon, lat, hole) for hole in polygon[1:]):
            return True
    return False


def _area(geometry: dict) -> float:
    total = 0.0
    polygons = geometry["coordinates"] if geometry.get("type") == "MultiPolygon" else [geometry["coordinates"]]
    for polygon in polygons:
        ring = polygon[0]
        total += abs(sum(ring[i][0] * ring[(i + 1) % len(ring)][1]
                         - ring[(i + 1) % len(ring)][0] * ring[i][1] for i in range(len(ring)))) / 2
    return total


def pick_extracts(index: dict, regions: list[Region]) -> list[dict]:
    """The smallest extract holding each Region, each named once, in the
    order the Regions need them."""
    picked: list[dict] = []
    for region in regions:
        s, w, n, e = region.bbox()
        corners = [(w, s), (e, s), (e, n), (w, n)]
        best = None
        for feature in index.get("features", []):
            geometry = feature.get("geometry")
            if not geometry or not feature.get("properties", {}).get("urls", {}).get("pbf"):
                continue
            if all(_in_multipolygon(lon, lat, geometry) for lon, lat in corners):
                area = _area(geometry)
                if best is None or area < best[0]:
                    best = (area, feature["properties"])
        if best is None:
            raise LookupError(f"no Geofabrik extract holds the Region {region.name!r}")
        if all(p["id"] != best[1]["id"] for p in picked):
            picked.append(best[1])
    return picked


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response, \
            tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as tmp:
        shutil.copyfileobj(response, tmp, 1 << 20)
    os.replace(tmp.name, dest)


def fetch_extracts(regions: list[Region], cache: Path, index_url: str = GEOFABRIK_INDEX,
                   refresh: bool = False, say=print) -> list[Path]:
    """The cached extracts for these Regions, fetched first if need be."""
    index_path = cache / "geofabrik-index.json"
    if refresh or not index_path.exists():
        say(f"fetching the extract index from {index_url}")
        _download(index_url, index_path)
    index = json.loads(index_path.read_text(encoding="utf-8"))
    paths = []
    for props in pick_extracts(index, regions):
        url = props["urls"]["pbf"]
        suffix = ".osm.pbf" if url.endswith(".osm.pbf") else Path(url).suffix or ".osm.pbf"
        path = cache / "extracts" / (props["id"].replace("/", "_") + suffix)
        if refresh or not path.exists():
            say(f"fetching extract {props['id']} from {url}")
            _download(url, path)
        say(f"extract {props['id']}: {path}")
        paths.append(path)
    return paths


# --------------------------------------------------------------------------
# Reading, clipped
# --------------------------------------------------------------------------

ROAD_CLASSES = {
    "motorway": "major", "trunk": "major", "primary": "major",
    "motorway_link": "major", "trunk_link": "major", "primary_link": "major",
    "secondary": "secondary", "secondary_link": "secondary",
    "tertiary": "tertiary", "tertiary_link": "tertiary",
    "residential": "minor", "unclassified": "minor", "living_street": "minor",
    "pedestrian": "minor", "road": "minor",
    "service": "service", "track": "service",
    "footway": "path", "path": "path", "cycleway": "path", "steps": "path", "bridleway": "path",
}

WATER_LINES = {"river": 3, "canal": 3, "stream": 1, "drain": 1, "ditch": 1}


@dataclass
class MapData:
    """What the renderer draws, in (lat, lon)."""
    roads: list[tuple[int, str, str, list]] = field(default_factory=list)       # id, class, name, line
    waterways: list[tuple[int, str, list]] = field(default_factory=list)       # id, kind, line
    water: list[tuple[int, list]] = field(default_factory=list)                # id, rings (outer first)
    parks: list[tuple[int, list]] = field(default_factory=list)
    buildings: list[tuple[int, list]] = field(default_factory=list)
    places: list[tuple[int, str, str, tuple]] = field(default_factory=list)    # id, kind, name, point
    pois: list[tuple[int, str, str, tuple]] = field(default_factory=list)      # id, icon, name, point

    def sort(self) -> None:
        for items in (self.roads, self.waterways, self.water, self.parks,
                      self.buildings, self.places, self.pois):
            items.sort(key=lambda item: item[0])


def poi_icon(tags) -> str | None:
    amenity = tags.get("amenity")
    if amenity == "place_of_worship":
        return "mosque" if tags.get("religion") == "muslim" else "worship"
    if amenity in ("hospital", "pharmacy", "fuel", "cafe", "restaurant", "school",
                   "bank", "atm", "parking", "toilets", "police", "post_office"):
        return {"atm": "bank"}.get(amenity, amenity)
    if tags.get("shop") in ("supermarket", "convenience"):
        return "shop"
    if tags.get("highway") == "bus_stop":
        return "bus"
    return None


def _box_meets(box, bbox) -> bool:
    s, w, n, e = bbox
    return not (box[2] < s or box[0] > n or box[3] < w or box[1] > e)


def _line_box(points) -> tuple[float, float, float, float]:
    lats = [p[0] for p in points]
    lons = [p[1] for p in points]
    return min(lats), min(lons), max(lats), max(lons)


def clip_box(regions: list[Region]) -> tuple[float, float, float, float]:
    """The Regions' bounding box, widened by a z14 tile's width each way, so
    what crosses a tile's edge is drawn whole and labels near it are kept."""
    boxes = [r.bbox() for r in regions]
    s = min(b[0] for b in boxes)
    w = min(b[1] for b in boxes)
    n = max(b[2] for b in boxes)
    e = max(b[3] for b in boxes)
    margin_lon = 360.0 / (1 << 14)
    margin_lat = margin_lon * math.cos(math.radians((s + n) / 2))
    return s - margin_lat, w - margin_lon, n + margin_lat, e + margin_lon


def read_extracts(paths: list[Path], regions: list[Region]) -> MapData:
    """Everything the renderer needs inside the Regions' clip box."""
    bbox = clip_box(regions)
    data = MapData()
    seen: set[tuple[str, int]] = set()

    for path in paths:
        for obj in osmium.FileProcessor(str(path)).with_areas():
            tags = obj.tags
            if obj.is_node():
                if not tags:
                    continue
                if ("n", obj.id) in seen:
                    continue
                loc = obj.location
                if not loc.valid():
                    continue
                point = (loc.lat, loc.lon)
                if not _box_meets((point[0], point[1], point[0], point[1]), bbox):
                    continue
                name = tags.get("name", "")
                place = tags.get("place")
                icon = poi_icon(tags)
                if place and name:
                    seen.add(("n", obj.id))
                    data.places.append((obj.id, place, name, point))
                elif icon:
                    seen.add(("n", obj.id))
                    data.pois.append((obj.id, icon, name, point))
            elif obj.is_way():
                highway = tags.get("highway")
                waterway = tags.get("waterway")
                if not (highway in ROAD_CLASSES or waterway in WATER_LINES):
                    continue
                if tags.get("area") == "yes" or ("w", obj.id) in seen:
                    continue
                try:
                    line = [(n.location.lat, n.location.lon) for n in obj.nodes]
                except osmium.InvalidLocationError:
                    line = [(n.location.lat, n.location.lon) for n in obj.nodes if n.location.valid()]
                if len(line) < 2 or not _box_meets(_line_box(line), bbox):
                    continue
                seen.add(("w", obj.id))
                if highway in ROAD_CLASSES:
                    data.roads.append((obj.id, ROAD_CLASSES[highway], tags.get("name", ""), line))
                else:
                    data.waterways.append((obj.id, waterway, line))
            elif obj.is_area():
                kind = None
                if (tags.get("natural") == "water" or tags.get("waterway") == "riverbank"
                        or tags.get("landuse") in ("reservoir", "basin")):
                    kind = data.water
                elif (tags.get("leisure") in ("park", "garden", "nature_reserve", "pitch")
                      or tags.get("landuse") in ("grass", "forest", "recreation_ground", "cemetery")
                      or tags.get("natural") == "wood"):
                    kind = data.parks
                elif tags.get("building") and tags.get("building") != "no":
                    kind = data.buildings
                if kind is None or ("a", obj.id) in seen:
                    continue
                for outer in obj.outer_rings():
                    ring = [(n.lat, n.lon) for n in outer if n.location.valid()]
                    if len(ring) < 3 or not _box_meets(_line_box(ring), bbox):
                        continue
                    rings = [ring]
                    for inner in obj.inner_rings(outer):
                        hole = [(n.lat, n.lon) for n in inner if n.location.valid()]
                        if len(hole) >= 3:
                            rings.append(hole)
                    kind.append((obj.id, rings))
                seen.add(("a", obj.id))
    data.sort()
    return data
