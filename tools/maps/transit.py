# SPDX-License-Identifier: BSD-3-Clause
"""A static transit network, baked into the Pack as vector overlay items
(the OVLY section, docs/maps/map-pack-format.md): the stops and the lines
inside the Regions, read from OpenStreetMap's route relations or from a
GTFS feed, so the Maps app draws them over the tiles and switches them off.

    thistle_maps.py build REGIONS.geojson --transit osm -o x.tmap
    thistle_maps.py build REGIONS.geojson --transit feed.zip -o x.tmap

What comes out is two overlays:

- "Transit lines": each stretch of track or road the same routes run
  along is one line, labelled with their names ("5, 23"), whichever way
  each runs and however many trips or variants share it. Rail first, then
  trams, ferries, buses; the longest first within each.
- "Transit stops": a stop, its platforms and stop positions are one point,
  named as they are and placed at their middle, with the routes calling at
  it. A square for a railway station, a ring for a tram stop, a triangle
  for a ferry, a dot for a bus stop; stations first.

An overlay holds 256 items and 16384 vertices (the map engine's limits,
map_overlay.h): what does not fit is left out, the least important first,
and the build says how many. Only what lies inside the Regions' tiles is
kept. Nothing here depends on the order the source lists things in, so the
same source gives the same bytes.
"""

from __future__ import annotations

import csv
import io
import math
import re
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from regions import Region, pack_tiles, world_pixel

MAX_ITEMS = 256
MAX_VERTICES = 16384
LABEL_MAX = 63          # bytes
VALUE_MAX = 159         # bytes of a property's value
MERGE_STOPS_M = 300     # stops of one name closer than this are one stop
SIMPLIFY_PX = 1.0       # at z17
SNAP = 1e5              # a vertex is snapped to 1e-5 degrees, about a metre

ICON_DOT, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE = 0, 1, 2, 3
KIND_POINT, KIND_LINE = 0, 1

# Modes, most important first, and how a stop of each is drawn.
MODES = ("train", "subway", "light_rail", "monorail", "tram", "ferry", "trolleybus", "bus")
MODE_ICON = {"train": ICON_SQUARE, "subway": ICON_SQUARE, "light_rail": ICON_SQUARE,
             "monorail": ICON_SQUARE, "tram": ICON_CIRCLE, "ferry": ICON_TRIANGLE,
             "trolleybus": ICON_DOT, "bus": ICON_DOT}

LINES_NAME = "Transit lines"
STOPS_NAME = "Transit stops"


@dataclass
class Item:
    kind: int
    icon: int
    points: list[tuple[float, float]]   # (lat, lon)
    label: str
    props: list[tuple[str, str]] = field(default_factory=list)


@dataclass
class Overlay:
    name: str
    items: list[Item]


@dataclass
class Network:
    overlays: list[Overlay]
    left_out: dict[str, int]            # overlay name: items left out

    def summary(self) -> str:
        parts = []
        for overlay in self.overlays:
            text = f"{len(overlay.items)} {overlay.name.split()[-1]}"
            if self.left_out.get(overlay.name):
                text += f" ({self.left_out[overlay.name]} left out, {MAX_ITEMS} at most)"
            parts.append(text)
        return "transit: " + ", ".join(parts)


class TransitError(ValueError):
    """A transit source the tool cannot read; the message says why."""


# --------------------------------------------------------------------------
# What both sources give: route segments and stops, before clipping
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class Route:
    mode: str
    ref: str


@dataclass
class RawStop:
    name: str
    lat: float
    lon: float
    routes: set[Route]
    station: bool = False


def _cut(text: str, limit: int) -> str:
    data = text.encode("utf-8")
    if len(data) <= limit:
        return text
    return data[:limit].decode("utf-8", "ignore")


def _natural(ref: str):
    return [(0, int(p), "") if p.isdigit() else (1, 0, p) for p in re.findall(r"\d+|\D+", ref)]


def _mode_rank(mode: str) -> int:
    return MODES.index(mode) if mode in MODES else len(MODES)


def _routes_label(routes) -> str:
    refs = sorted({r.ref for r in routes if r.ref}, key=_natural)
    return ", ".join(refs)


def _modes_text(routes) -> str:
    return ", ".join(sorted({r.mode for r in routes}, key=_mode_rank))


class _Inside:
    """Whether a point is on one of the Pack's z14 tiles: inside a Region,
    near enough."""

    def __init__(self, regions: list[Region]):
        self.tiles = {(x, y) for z, x, y in pack_tiles(regions) if z == 14}

    def __call__(self, lat: float, lon: float) -> bool:
        x, y = world_pixel(lat, lon, 14)
        return (int(x // 256), int(y // 256)) in self.tiles


def _snap(p: tuple[float, float]) -> tuple[int, int]:
    return (round(p[0] * SNAP), round(p[1] * SNAP))


def _unsnap(k: tuple[int, int]) -> tuple[float, float]:
    return (k[0] / SNAP, k[1] / SNAP)


class _Graph:
    """Undirected segments, each with the routes along it."""

    def __init__(self) -> None:
        self.edges: dict[tuple, set[Route]] = {}

    def add_line(self, points: list[tuple[float, float]], route: Route) -> None:
        keys = [_snap(p) for p in points]
        for a, b in zip(keys, keys[1:]):
            if a == b:
                continue
            edge = (a, b) if a < b else (b, a)
            self.edges.setdefault(edge, set()).add(route)

    def clip(self, inside: _Inside) -> None:
        def keep(edge):
            a, b = _unsnap(edge[0]), _unsnap(edge[1])
            mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            return inside(*a) or inside(*b) or inside(*mid)
        self.edges = {e: r for e, r in self.edges.items() if keep(e)}

    def chains(self) -> list[tuple[list[tuple[int, int]], frozenset[Route]]]:
        """The segments joined into lines: a line runs on through a vertex
        where exactly two segments meet with the same routes."""
        around: dict[tuple, list[tuple]] = {}
        for edge in self.edges:
            for v in edge:
                around.setdefault(v, []).append(edge)
        for v in around:
            around[v].sort()

        def runs_on(v, edge) -> bool:
            es = around[v]
            return len(es) == 2 and self.edges[es[0]] == self.edges[es[1]]

        used: set[tuple] = set()
        out = []

        def walk(start, edge):
            routes = self.edges[edge]
            line = [start]
            v = start
            while True:
                used.add(edge)
                w = edge[1] if edge[0] == v else edge[0]
                line.append(w)
                if w == start or not runs_on(w, edge):
                    break
                nxt = around[w][0] if around[w][1] == edge else around[w][1]
                if nxt in used:
                    break
                v, edge = w, nxt
            out.append((line, frozenset(routes)))

        for v in sorted(around):
            for edge in around[v]:
                if edge not in used and not runs_on(v, edge):
                    walk(v, edge)
        for edge in sorted(self.edges):        # loops, with no end
            if edge not in used:
                walk(edge[0], edge)
        return out


def _simplify(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Douglas-Peucker, SIMPLIFY_PX at z17."""
    if len(points) <= 2:
        return points
    px = [world_pixel(lat, lon, 17) for lat, lon in points]
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        i, j = stack.pop()
        (x1, y1), (x2, y2) = px[i], px[j]
        dx, dy = x2 - x1, y2 - y1
        norm = math.hypot(dx, dy)
        best, at = -1.0, -1
        for k in range(i + 1, j):
            x, y = px[k]
            d = (abs(dy * (x - x1) - dx * (y - y1)) / norm) if norm else math.hypot(x - x1, y - y1)
            if d > best:
                best, at = d, k
        if at >= 0 and best > SIMPLIFY_PX:
            keep[at] = True
            stack += [(i, at), (at, j)]
    return [p for p, k in zip(points, keep) if k]


def _length(points) -> float:
    px = [world_pixel(lat, lon, 17) for lat, lon in points]
    return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(px, px[1:]))


def _merge_stops(stops: list[RawStop]) -> list[RawStop]:
    """Stops of one name within MERGE_STOPS_M of each other: one stop at
    their middle. A stop with no name stays alone."""
    from regions import distance_m

    groups: list[list[RawStop]] = []
    for stop in sorted(stops, key=lambda s: (s.name, s.lat, s.lon)):
        for group in groups:
            if stop.name and group[0].name == stop.name and \
                    distance_m((group[0].lat, group[0].lon), (stop.lat, stop.lon)) <= MERGE_STOPS_M:
                group.append(stop)
                break
        else:
            groups.append([stop])
    out = []
    for group in groups:
        routes = set().union(*(s.routes for s in group))
        out.append(RawStop(group[0].name, sum(s.lat for s in group) / len(group),
                           sum(s.lon for s in group) / len(group), routes,
                           any(s.station for s in group)))
    return out


def _cap(items: list[Item]) -> tuple[list[Item], int]:
    kept, vertices = [], 0
    for item in items:
        if len(kept) == MAX_ITEMS or vertices + len(item.points) > MAX_VERTICES:
            continue
        kept.append(item)
        vertices += len(item.points)
    return kept, len(items) - len(kept)


def make_network(graph: _Graph, stops: list[RawStop], regions: list[Region]) -> Network:
    inside = _Inside(regions)
    graph.clip(inside)

    lines = []
    for keys, routes in graph.chains():
        points = _simplify([_unsnap(k) for k in keys])
        best = min(_mode_rank(r.mode) for r in routes)
        label = _cut(_routes_label(routes), LABEL_MAX)
        lines.append((best, -_length(points), label, points, routes))
    lines.sort(key=lambda t: (t[0], t[1], t[2], t[3]))
    line_items = [Item(KIND_LINE, 0, points, label,
                       [("mode", _cut(_modes_text(routes), VALUE_MAX))])
                  for _r, _l, label, points, routes in lines]

    kept_stops = [s for s in _merge_stops(stops) if s.routes and inside(s.lat, s.lon)]

    def stop_key(s: RawStop):
        rank = min(_mode_rank(r.mode) for r in s.routes)
        return (rank, not s.station, -len(s.routes), s.name, s.lat, s.lon)

    stop_items = []
    for s in sorted(kept_stops, key=stop_key):
        rank_mode = min((r.mode for r in s.routes), key=_mode_rank)
        props = [("lines", _cut(_routes_label(s.routes), VALUE_MAX)),
                 ("mode", _cut(_modes_text(s.routes), VALUE_MAX))]
        if not props[0][1]:
            props = props[1:]
        stop_items.append(Item(KIND_POINT, MODE_ICON.get(rank_mode, ICON_DOT),
                               [(s.lat, s.lon)], _cut(s.name, LABEL_MAX), props))

    line_items, lines_out = _cap(line_items)
    stop_items, stops_out = _cap(stop_items)
    return Network([Overlay(LINES_NAME, line_items), Overlay(STOPS_NAME, stop_items)],
                   {LINES_NAME: lines_out, STOPS_NAME: stops_out})


# --------------------------------------------------------------------------
# OpenStreetMap: route relations
# --------------------------------------------------------------------------

OSM_MODES = {"train": "train", "railway": "train", "subway": "subway",
             "light_rail": "light_rail", "monorail": "monorail", "tram": "tram",
             "ferry": "ferry", "trolleybus": "trolleybus", "bus": "bus"}


def read_osm(paths: list[Path], regions: list[Region]) -> Network:
    """The route relations of the extracts (type=route, route=bus, tram,
    subway, train, light_rail, monorail, ferry, trolleybus): their ways are
    the lines, their stop and platform nodes the stops."""
    import osmium

    way_routes: dict[int, set[Route]] = {}
    node_routes: dict[int, set[Route]] = {}
    for path in paths:
        for rel in osmium.FileProcessor(str(path), osmium.osm.RELATION):
            tags = rel.tags
            mode = OSM_MODES.get(tags.get("route", ""))
            if tags.get("type") != "route" or not mode:
                continue
            ref = tags.get("ref") or tags.get("name") or ""
            route = Route(mode, ref)
            for m in rel.members:
                role = m.role or ""
                if m.type == "w" and not role.startswith("platform"):
                    way_routes.setdefault(m.ref, set()).add(route)
                elif m.type == "n" and (role.startswith("stop") or role.startswith("platform")):
                    node_routes.setdefault(m.ref, set()).add(route)

    graph = _Graph()
    stops: dict[int, RawStop] = {}
    seen_ways: set[int] = set()
    for path in paths:
        processor = osmium.FileProcessor(str(path), osmium.osm.NODE | osmium.osm.WAY)
        for obj in processor.with_locations():
            if obj.is_node():
                if obj.id in node_routes and obj.id not in stops and obj.location.valid():
                    tags = obj.tags
                    stops[obj.id] = RawStop(tags.get("name", ""), obj.location.lat,
                                            obj.location.lon, set(node_routes[obj.id]),
                                            tags.get("railway") in ("station", "halt")
                                            or tags.get("public_transport") == "station")
            elif obj.is_way() and obj.id in way_routes and obj.id not in seen_ways:
                seen_ways.add(obj.id)
                line = [(n.location.lat, n.location.lon) for n in obj.nodes if n.location.valid()]
                for route in way_routes[obj.id]:
                    graph.add_line(line, route)
    return make_network(graph, [stops[k] for k in sorted(stops)], regions)


# --------------------------------------------------------------------------
# GTFS
# --------------------------------------------------------------------------

def gtfs_mode(route_type: str) -> str:
    """GTFS's route_type, basic and extended, as a mode."""
    try:
        t = int(route_type)
    except ValueError:
        return "bus"
    basic = {0: "tram", 1: "subway", 2: "train", 3: "bus", 4: "ferry", 5: "tram",
             7: "train", 11: "trolleybus", 12: "monorail"}
    if t in basic:
        return basic[t]
    if 100 <= t < 200 or 300 <= t < 400:
        return "train"
    if 400 <= t < 500:
        return "subway"
    if 700 <= t < 800:
        return "bus"
    if 800 <= t < 900:
        return "trolleybus"
    if 900 <= t < 1000:
        return "tram"
    if 1000 <= t < 1100 or 1200 <= t < 1300:
        return "ferry"
    return "bus"


class _Feed:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.zip = zipfile.ZipFile(self.path) if self.path.is_file() else None
        if not self.zip and not self.path.is_dir():
            raise TransitError(f"no GTFS feed at {path}: give a .zip or a folder")

    def has(self, name: str) -> bool:
        if self.zip:
            return name in self.zip.namelist()
        return (self.path / name).is_file()

    def rows(self, name: str):
        if not self.has(name):
            raise TransitError(f"the GTFS feed {self.path} has no {name}")
        raw = self.zip.open(name) if self.zip else open(self.path / name, "rb")
        with raw, io.TextIOWrapper(raw, encoding="utf-8-sig", newline="") as text:
            for row in csv.DictReader(text):
                yield {k.strip(): (v or "").strip() for k, v in row.items() if k}


def read_gtfs(path: Path, regions: list[Region]) -> Network:
    """A GTFS feed (a .zip or a folder): shapes.txt draws each trip's line;
    a route with no shape is drawn stop to stop along its first trip. A
    stop is its parent station with all its platforms."""
    feed = _Feed(path)
    routes: dict[str, Route] = {}
    for row in feed.rows("routes.txt"):
        ref = row.get("route_short_name") or row.get("route_long_name") or row.get("route_id", "")
        routes[row["route_id"]] = Route(gtfs_mode(row.get("route_type", "3")), ref)

    trip_route: dict[str, Route] = {}
    shape_routes: dict[str, set[Route]] = {}
    unshaped: dict[Route, str] = {}         # a route with no shape: its first trip
    for row in feed.rows("trips.txt"):
        route = routes.get(row.get("route_id", ""))
        if not route:
            continue
        trip_route[row["trip_id"]] = route
        shape = row.get("shape_id", "")
        if shape:
            shape_routes.setdefault(shape, set()).add(route)
    shaped = set().union(*shape_routes.values()) if shape_routes else set()
    for trip in sorted(trip_route):
        route = trip_route[trip]
        if route not in shaped and route not in unshaped:
            unshaped[route] = trip

    stops: dict[str, dict] = {}
    for row in feed.rows("stops.txt"):
        try:
            lat, lon = float(row["stop_lat"]), float(row["stop_lon"])
        except (KeyError, ValueError):
            continue
        stops[row["stop_id"]] = {"name": row.get("stop_name", ""), "lat": lat, "lon": lon,
                                 "type": row.get("location_type", "") or "0",
                                 "parent": row.get("parent_station", "")}

    def top(stop_id: str) -> str:
        parent = stops[stop_id]["parent"]
        return parent if parent in stops else stop_id

    stop_routes: dict[str, set[Route]] = {}
    sequences: dict[str, list[tuple[int, str]]] = {t: [] for t in unshaped.values()}
    if feed.has("stop_times.txt"):
        for row in feed.rows("stop_times.txt"):
            trip, stop_id = row.get("trip_id", ""), row.get("stop_id", "")
            route = trip_route.get(trip)
            if not route or stop_id not in stops:
                continue
            stop_routes.setdefault(top(stop_id), set()).add(route)
            if trip in sequences:
                try:
                    sequences[trip].append((int(row.get("stop_sequence", "0")), stop_id))
                except ValueError:
                    pass

    graph = _Graph()
    if shape_routes and feed.has("shapes.txt"):
        shapes: dict[str, list[tuple[int, float, float]]] = {}
        for row in feed.rows("shapes.txt"):
            sid = row.get("shape_id", "")
            if sid not in shape_routes:
                continue
            try:
                shapes.setdefault(sid, []).append((int(row["shape_pt_sequence"]),
                                                   float(row["shape_pt_lat"]),
                                                   float(row["shape_pt_lon"])))
            except (KeyError, ValueError):
                continue
        for sid in sorted(shapes):
            line = [(lat, lon) for _seq, lat, lon in sorted(shapes[sid])]
            for route in shape_routes[sid]:
                graph.add_line(line, route)
    for route, trip in sorted(unshaped.items(), key=lambda kv: kv[1]):
        seq = [stops[s] for _n, s in sorted(sequences.get(trip, []))]
        graph.add_line([(s["lat"], s["lon"]) for s in seq], route)

    raw = []
    for stop_id in sorted(stop_routes):
        s = stops[stop_id]
        raw.append(RawStop(s["name"], s["lat"], s["lon"], stop_routes[stop_id], s["type"] == "1"))
    return make_network(graph, raw, regions)


def read_transit(source: str, extracts: list[Path], regions: list[Region]) -> Network:
    """--transit's SOURCE: "osm" for the extracts' route relations, else a
    GTFS feed's path."""
    if source == "osm":
        return read_osm(extracts, regions)
    return read_gtfs(Path(source), regions)
