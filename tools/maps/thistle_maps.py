#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The Map Pack tool: builds the one file the Maps app reads from the
Regions a user drew.

    thistle_maps.py build REGIONS.geojson -o my-places.tmap
    thistle_maps.py build REGIONS.geojson --extract tunisia.osm.pbf -o my-places.tmap
    thistle_maps.py estimate REGIONS.geojson [--json]

`build` fetches the Geofabrik extract that holds the Regions (once; it is
cached, so later builds need no network), clips it to them, draws every
tile each Region needs from z14 to its own deepest level, and writes the
Pack. Copy the Pack to the card's /sdcard/maps/.

`estimate` says how big the Pack would be, without building it.

The browser mode, where Regions are drawn on a real map, is ticket 05's.
docs/maps/map-pack-format.md has the Regions file and the Pack's format.
Needs Pillow (with its raqm, for Arabic) and osmium: pip install -r
tools/requirements.txt.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from regions import RegionsError, load_regions, pack_tiles  # noqa: E402


def parse_size(text: str) -> int:
    """'1GB', '500 MB', '2.5G' or a number of bytes; units are powers of 1024."""
    t = text.strip().upper().replace(" ", "").removesuffix("B")
    units = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}
    scale = units.get(t[-1:], 1)
    try:
        value = float(t[:-1] if t[-1:] in units else t)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a size: {text!r}") from None
    if value <= 0:
        raise argparse.ArgumentTypeError(f"not a size: {text!r}")
    return int(value * scale)


def human_size(n: int) -> str:
    for unit, scale in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= scale:
            return f"{n / scale:.3g} {unit}"
    return f"{n} bytes"


def find_extracts(args, regions) -> list[Path]:
    """The extracts given, or those Geofabrik has for the Regions, fetched
    into the cache once."""
    import osm_source

    if args.extract:
        return [Path(p) for p in args.extract]
    cache = Path(args.cache) if args.cache else osm_source.default_cache()
    return osm_source.fetch_extracts(regions, cache, args.geofabrik_index, args.refresh)


def measure(args, regions):
    import estimate
    import osm_source
    import render

    fonts = render.Fonts(render.find_font(args.font))
    data = osm_source.read_extracts(find_extracts(args, regions), regions)
    return estimate.measure_pack(regions, data, fonts)


def cmd_estimate(args) -> int:
    import estimate

    regions = load_regions(args.regions)
    if args.extract or args.measure:
        result = measure(args, regions)
    else:
        result = estimate.estimate_pack(regions)
    if args.json:
        print(json.dumps(result.as_json(args.budget)))
    else:
        levels = ", ".join(f"z{z} {n}" for z, n in result.tiles_per_zoom.items())
        how = "measured from the extract" if result.measured else "priced as a city"
        print(f"{result.tiles} tiles ({levels}), about {human_size(result.bytes)}, {how}")
        if result.over_budget(args.budget):
            print(f"more than the {human_size(args.budget)} budget")
    return 0


def cmd_build(args) -> int:
    import osm_source
    import pack_writer
    import render

    regions = load_regions(args.regions)
    fonts = render.Fonts(render.find_font(args.font))
    extracts = find_extracts(args, regions)
    tiles = pack_tiles(regions)
    print(f"{len(regions)} regions, {len(tiles)} tiles")
    data = osm_source.read_extracts(extracts, regions)
    print(f"read {len(data.roads)} roads, {len(data.water)} water areas, {len(data.parks)} parks, "
          f"{len(data.buildings)} buildings, {len(data.places)} places, {len(data.pois)} places of interest")

    def progress(done: int, total: int) -> None:
        if not args.quiet and (done == total or done % 16 == 0):
            print(f"  drawn {done} of {total} blocks", flush=True)

    bitmaps = render.render_tiles(data, tiles, fonts, progress)
    size = pack_writer.write_pack(args.output, regions, bitmaps)
    print(f"wrote {args.output}: {len(bitmaps)} tiles, {size} bytes")
    return 0


DEFAULT_BUDGET = 1 << 30  # the 1 GB warning of the estimate and the browser mode


def add_source_arguments(parser) -> None:
    """Where the map data comes from, for whatever reads it."""
    parser.add_argument("--extract", action="append",
                        help="an OpenStreetMap extract (.osm.pbf or .osm) to read instead of "
                             "fetching one; may be given more than once")
    parser.add_argument("--cache", help="where fetched extracts are kept "
                                        "(default ~/.cache/thistle-maps)")
    parser.add_argument("--geofabrik-index", default="https://download.geofabrik.de/index-v1.json",
                        help="Geofabrik's extract index")
    parser.add_argument("--refresh", action="store_true",
                        help="fetch the index and the extracts again")
    parser.add_argument("--font", help="the TTF labels are drawn in (default DejaVu Sans)")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="build a Map Pack from a Regions file")
    build.add_argument("regions", help="the Regions file (GeoJSON)")
    build.add_argument("-o", "--output", required=True, help="the Pack to write (.tmap)")
    add_source_arguments(build)
    build.add_argument("--quiet", action="store_true")
    build.set_defaults(run=cmd_build)

    est = sub.add_parser("estimate", help="how big a Pack of these Regions would be")
    est.add_argument("regions")
    est.add_argument("--json", action="store_true")
    est.add_argument("--measure", action="store_true",
                     help="draw a sample of the tiles from the extract (fetched as build "
                          "fetches it) rather than pricing them as a city's; --extract "
                          "implies it")
    est.add_argument("--budget", type=parse_size, default=DEFAULT_BUDGET,
                     help="warn above this size (default 1GB)")
    add_source_arguments(est)
    est.set_defaults(run=cmd_estimate)

    args = parser.parse_args(argv)
    try:
        return args.run(args)
    except RegionsError as exc:
        print(f"thistle_maps: {exc}", file=sys.stderr)
        return 2
    except (LookupError, OSError, RuntimeError) as exc:
        print(f"thistle_maps: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
