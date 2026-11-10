#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Writes the Map Pack fixture: a small made-up OpenStreetMap extract
(fixture.osm) and the Regions file drawn over it (fixture-regions.geojson).

    python3 tools/maps/testdata/make_fixture.py

Nothing in it comes from OpenStreetMap: every street, lake and shop is
placed here, in metres from two centres in Tunis, so the fixture carries no
one's data and changes only when this script does. The golden Pack,
fixture.tmap, is built from these two files by the real tool
(test_thistle_maps.py says how to rebuild it).

Region "Medina" is a circle of 350 m around (36.8000, 10.1800), as deep as
z17; region "Lac Nord" a freehand polygon around (36.8150, 10.2000), as deep
as z15. The quarter's name, "محمد", sits alone 120 m north-east of the
Medina's centre, with nothing else drawn near it: a test finds it on the
screen and checks that its letters are joined.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from xml.sax.saxutils import quoteattr

HERE = Path(__file__).resolve().parent

MEDINA = (36.8000, 10.1800)
LAC_NORD = (36.8150, 10.2000)


def at(centre: tuple[float, float], east_m: float, north_m: float) -> tuple[float, float]:
    """The point east_m east and north_m north of centre, as (lat, lon)."""
    lat, lon = centre
    dlat = north_m / 111_320.0
    dlon = east_m / (111_320.0 * math.cos(math.radians(lat)))
    return (round(lat + dlat, 7), round(lon + dlon, 7))


class Extract:
    def __init__(self) -> None:
        self.nodes: list[tuple[int, tuple[float, float], dict[str, str]]] = []
        self.ways: list[tuple[int, list[int], dict[str, str]]] = []
        self.relations: list[tuple[int, list[tuple[str, int, str]], dict[str, str]]] = []
        self._next_node = 1000
        self._next_way = 2000
        self._next_relation = 3000

    def node(self, point: tuple[float, float], tags: dict[str, str] | None = None) -> int:
        self._next_node += 1
        self.nodes.append((self._next_node, point, tags or {}))
        return self._next_node

    def way(self, points: list[tuple[float, float]], tags: dict[str, str], closed: bool = False) -> int:
        refs = [self.node(p) for p in points]
        if closed:
            refs.append(refs[0])
        self._next_way += 1
        self.ways.append((self._next_way, refs, tags))
        return self._next_way

    def relation(self, members: list[tuple[str, int, str]], tags: dict[str, str]) -> int:
        self._next_relation += 1
        self.relations.append((self._next_relation, members, tags))
        return self._next_relation

    def xml(self) -> str:
        lines = ['<?xml version="1.0" encoding="UTF-8"?>',
                 '<osm version="0.6" generator="thistle-maps make_fixture.py">']

        def tag_lines(tags: dict[str, str]) -> list[str]:
            return [f"    <tag k={quoteattr(k)} v={quoteattr(v)}/>" for k, v in tags.items()]

        for nid, (lat, lon), tags in self.nodes:
            head = f'  <node id="{nid}" version="1" lat="{lat:.7f}" lon="{lon:.7f}"'
            if tags:
                lines.append(head + ">")
                lines += tag_lines(tags)
                lines.append("  </node>")
            else:
                lines.append(head + "/>")
        for wid, refs, tags in self.ways:
            lines.append(f'  <way id="{wid}" version="1">')
            lines += [f'    <nd ref="{r}"/>' for r in refs]
            lines += tag_lines(tags)
            lines.append("  </way>")
        for rid, members, tags in self.relations:
            lines.append(f'  <relation id="{rid}" version="1">')
            lines += [f'    <member type="{t}" ref="{r}" role="{role}"/>' for t, r, role in members]
            lines += tag_lines(tags)
            lines.append("  </relation>")
        lines.append("</osm>")
        return "\n".join(lines) + "\n"


def rect(centre, x0, y0, x1, y1):
    return [at(centre, x0, y0), at(centre, x1, y0), at(centre, x1, y1), at(centre, x0, y1)]


def build() -> Extract:
    e = Extract()
    m = MEDINA

    # Roads, one of each class the renderer draws, none near the quarter's
    # name in the north-east.
    e.way([at(m, -700, -60), at(m, 0, -60), at(m, 700, -60)],
          {"highway": "primary", "name": "Avenue de France"})
    e.way([at(m, -100, -700), at(m, -100, 0), at(m, -100, 700)],
          {"highway": "secondary", "name": "نهج الحرية"})
    e.way([at(m, -500, 180), at(m, -150, 180)],
          {"highway": "residential", "name": "Rue de Rome"})
    e.way([at(m, 250, -600), at(m, 250, -100)],
          {"highway": "tertiary", "name": "Rue d'Alger"})
    e.way([at(m, -450, -500), at(m, -200, -250)],
          {"highway": "service"})
    e.way([at(m, -600, 400), at(m, -300, 600)],
          {"highway": "footway"})
    e.way([at(m, -900, 300), at(m, -900, -300)],
          {"highway": "motorway", "name": "A1"})

    # A lake, as a multipolygon with an island, and a stream into it.
    outer = e.way([at(m, -420, -420), at(m, 80, -420), at(m, 80, -260), at(m, -420, -260)],
                  {}, closed=True)
    island = e.way([at(m, -220, -370), at(m, -160, -370), at(m, -160, -320), at(m, -220, -320)],
                   {}, closed=True)
    e.relation([("way", outer, "outer"), ("way", island, "inner")],
               {"type": "multipolygon", "natural": "water", "name": "Lac"})
    e.way([at(m, 300, -700), at(m, 80, -340)], {"waterway": "stream"})

    # A park, hatched.
    e.way(rect(m, 260, 100, 500, 350), {"leisure": "park", "name": "Jardin"}, closed=True)

    # Buildings south of the avenue: drawn at z16 and z17 only.
    for i in range(5):
        x0 = -60 + i * 45
        e.way(rect(m, x0, -150, x0 + 30, -110), {"building": "yes"}, closed=True)

    # Points of interest.
    e.node(at(m, -250, 50), {"amenity": "place_of_worship", "religion": "muslim",
                             "name": "جامع الزيتونة"})
    e.node(at(m, 150, -200), {"amenity": "pharmacy", "name": "Pharmacie"})
    e.node(at(m, -300, -150), {"amenity": "hospital", "name": "Hôpital"})
    e.node(at(m, 350, -120), {"amenity": "fuel"})
    e.node(at(m, 0, 300), {"amenity": "cafe", "name": "Café"})
    e.node(at(m, -330, 280), {"amenity": "school"})

    # The quarter's name, alone: its letters join into one shape.
    e.node(at(m, 120, 120), {"place": "neighbourhood", "name": "محمد"})

    # Lac Nord: a road and a building, which no level of its own shows.
    n = LAC_NORD
    e.way([at(n, -400, -20), at(n, 400, 20)],
          {"highway": "tertiary", "name": "Rue de Marseille"})
    e.way(rect(n, 30, 40, 70, 80), {"building": "yes"}, closed=True)
    e.node(at(n, 0, 150), {"place": "suburb", "name": "Les Berges"})

    # Far from both Regions: clipped away.
    e.way([at(m, 0, -9000), at(m, 500, -9000)], {"highway": "primary", "name": "Far Road"})
    return e


def regions() -> dict:
    polygon = [at(LAC_NORD, dx, dy) for dx, dy in
               ((-300, -200), (50, -260), (320, -120), (280, 210), (-60, 300), (-320, 120))]
    ring = [[lon, lat] for lat, lon in polygon]
    ring.append(ring[0])
    return {
        "type": "FeatureCollection",
        "thistle_regions": 1,
        "features": [
            {"type": "Feature",
             "properties": {"name": "Medina", "radius_m": 350, "deepest_zoom": 17},
             "geometry": {"type": "Point", "coordinates": [MEDINA[1], MEDINA[0]]}},
            {"type": "Feature",
             "properties": {"name": "Lac Nord", "deepest_zoom": 15},
             "geometry": {"type": "Polygon", "coordinates": [ring]}},
        ],
    }


def main() -> None:
    (HERE / "fixture.osm").write_text(build().xml(), encoding="utf-8")
    (HERE / "fixture-regions.geojson").write_text(
        json.dumps(regions(), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
