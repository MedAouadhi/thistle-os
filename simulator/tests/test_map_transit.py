#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""A static transit network baked into the Map Pack (the tool's --transit,
the Pack's OVLY section), on the simulated T-Deck Max's Maps app: its lines
and stops are drawn over the tiles as two overlays, switched off and on in
the Layers panel, and a tapped stop or line shows its card.

The Pack is the tool's golden transit fixture,
tools/maps/testdata/fixture-transit.tmap: the golden fixture's tiles with
make_fixture.py's network baked in (tram T1 140 m north of the Medina's
centre, with Place Barcelone and Bab Bhar; buses 5 and 23 along the Avenue
de France, 60 m south, calling at its stop). Where the network is, is
worked out here from make_fixture.py's metres and Web Mercator; what the
screen should show elsewhere is the Pack's tiles, read by the tool tests'
own reader (test_map_overlays.map_differs).

The app logs:

    (maps) opened fixture.tmap: format 1.1, 2 regions, 22 tiles
    (maps) Pack overlay Transit lines: 2 items
    (maps) Pack overlay Transit stops: 3 items
    (maps) overlay Transit lines off
    (maps) info card: Bab Bhar; lines: T1; mode: tram
    (maps) the Pack's overlays are damaged: left out
"""

from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_maps  # noqa: E402
import test_map_overlays  # noqa: E402
from test_maps import dark, need, world_pixel  # noqa: E402
from test_map_overlays import Z, box, logged_at, map_differs, opening_view, tap  # noqa: E402

TRANSIT = test_maps.ROOT / "tools" / "maps" / "testdata" / "fixture-transit.tmap"
MEDINA = (36.8000, 10.1800)


def at(east_m: float, north_m: float) -> tuple[float, float]:
    """make_fixture.py's at(MEDINA, ...)."""
    return (MEDINA[0] + north_m / 111_320.0,
            MEDINA[1] + east_m / (111_320.0 * math.cos(math.radians(MEDINA[0]))))


def screen(east_m: float, north_m: float) -> tuple[int, int]:
    """Where a place is on the screen in the opening view."""
    vx, vy, _w, _h, left, top = opening_view()
    gx, gy = world_pixel(*at(east_m, north_m), Z)
    return vx + math.floor(gx) - left, vy + math.floor(gy) - top


def run(pack: bytes = None, steps: tuple[str, ...] = (), timeout_ms: int = 3500):
    return test_maps.run_app(TRANSIT.read_bytes() if pack is None else pack, steps=steps,
                             timeout_ms=timeout_ms)


# --------------------------------------------------------------------------
# Cases
# --------------------------------------------------------------------------

def line_dark(shot, y: int, xs) -> list[int]:
    """The x of xs where row y, or the row above or below it, is not ink."""
    return [x for x in xs if not any(dark(shot, x, y + d) for d in (-1, 0, 1))]


def layer_rows() -> tuple[tuple[int, int], tuple[int, int], tuple[int, int]]:
    """Where the Layers button and the panel's two rows are."""
    probe = run(steps=("wait",), timeout_ms=4500)
    layers = logged_at(probe.log, "layers")
    probe = run(steps=(tap(layers),), timeout_ms=4500)
    return layers, logged_at(probe.log, "layer Transit lines"), logged_at(probe.log,
                                                                          "layer Transit stops")


def the_network_is_drawn_over_the_map() -> None:
    shown = run()
    need(shown.log, "(maps) opened fixture.tmap: format 1.1, 2 regions, 22 tiles")
    need(shown.log, "(maps) Pack overlay Transit lines: 2 items")
    need(shown.log, "(maps) Pack overlay Transit stops: 3 items")
    need(shown.log, "(map_view) overlays drawn: 2 on, 5 items in view, 0 left out to declutter")
    # A tram stop is a ring, a bus stop a dot: drawn over the map around
    # the place (a stop's icon is 15 px).
    differ = map_differs(shown)
    for name, place in (("Bab Bhar", (100, 140)), ("Place Barcelone", (-200, 140)),
                        ("Avenue de France", (-20, -50))):
        x, y = screen(*place)
        ink = [(px, py) for px, py in differ if abs(px - x) <= 7 and abs(py - y) <= 7
               and dark(shown.shot, px, py)]
        if len(ink) < 12:
            raise AssertionError(f"no stop drawn for {name} at {x},{y}")
    # Each line shows where no stop's label covers it: west of the first.
    _x, ty = screen(0, 140)
    _x, by = screen(0, -60)
    if line_dark(shown.shot, by, range(50, 100, 2)):
        raise AssertionError(f"the buses' line is not drawn at y {by}")
    # Nothing is drawn far from the network: the rows halfway between the
    # two lines are the Pack's.
    mid = (ty + by) // 2
    stray = {p for p in differ if abs(p[1] - mid) < 15}
    if stray:
        raise AssertionError(f"something was drawn between the lines, at {box(stray)}")


def the_lines_cross_the_view_with_the_stops_off() -> None:
    """With the stops (and their labels) off, both lines run the view's
    width, as straight as the tram's track and the avenue are."""
    layers, _lines, stops = layer_rows()
    shown = run(steps=(tap(layers), tap(stops), tap(layers)), timeout_ms=7500)
    need(shown.log, "(maps) overlay Transit stops off")
    need(shown.log, "(map_view) overlays drawn: 1 on")
    _vx, _vy, w, _h, _l, _t = opening_view()
    for what, north in (("tram", 140), ("buses", -60)):
        _x, y = screen(0, north)
        missing = line_dark(shown.shot, y, range(50, w - 50, 2))
        if missing:
            raise AssertionError(f"the {what}' line is not drawn at y {y}, x {missing[:6]}")
        above = [x for x in range(50, w - 50, 2) if dark(shown.shot, x, y - 6)]
        if len(above) > (w - 100) // 4:
            raise AssertionError(f"the {what}' line is wider than 7 px at y {y}")


def the_network_switches_off_and_on() -> None:
    """Both overlays off from the Layers panel: the map is the Pack's
    tiles, pixel for pixel. On again, the stops are back."""
    layers, lines, stops = layer_rows()
    off = run(steps=(tap(layers), tap(lines), tap(stops), tap(layers)), timeout_ms=9000)
    need(off.log, "(maps) overlay Transit lines off")
    need(off.log, "(maps) overlay Transit stops off")
    need(off.log, "(map_view) overlays drawn: 0 on")
    if map_differs(off):
        raise AssertionError(f"with the network off, the map differs from the Pack at "
                             f"{box(map_differs(off))}")
    on = run(steps=(tap(layers), tap(stops), tap(stops), tap(layers)), timeout_ms=9000)
    need(on.log, "(maps) overlay Transit stops on")
    need(on.log, "(map_view) overlays drawn: 2 on")
    x, y = screen(-20, -50)
    if not dark(on.shot, x, y):  # a dot
        raise AssertionError("switched on again, the Avenue de France stop is not drawn")


def a_tapped_stop_shows_its_lines() -> None:
    shown = run(steps=(tap(screen(100, 140)),), timeout_ms=4500)
    need(shown.log, '"Transit stops" item')
    need(shown.log, "(maps) info card: Bab Bhar; lines: T1; mode: tram")


def a_tapped_line_shows_its_routes() -> None:
    # On the avenue, 50 px west of its stop.
    _x, y = screen(-20, -60)
    shown = run(steps=(f"60,{y}",), timeout_ms=4500)
    need(shown.log, '"Transit lines" item')
    need(shown.log, "(maps) info card: 5, 23; mode: bus")


def ovly(pack: bytes) -> tuple[int, int, int]:
    """OVLY's directory entry: its index, offset and length."""
    count = struct.unpack_from("<H", pack, 18)[0]
    for i in range(count):
        tag, offset, length, _crc = struct.unpack_from("<4sIII", pack, 32 + 16 * i)
        if tag == b"OVLY":
            return i, offset, length
    raise AssertionError("the transit fixture has no OVLY")


def damaged_overlays_leave_the_map() -> None:
    """A byte of OVLY changed: its CRC no longer holds, and the Pack opens
    with no overlays, its tiles drawn as ever."""
    data = bytearray(TRANSIT.read_bytes())
    _i, offset, length = ovly(bytes(data))
    data[offset + length // 2] ^= 0x40
    shown = run(bytes(data))
    need(shown.log, "(maps) opened fixture.tmap: format 1.1")
    need(shown.log, "(maps) the Pack's overlays are damaged: left out")
    if "Pack overlay" in shown.log:
        raise AssertionError("a damaged overlay was shown")
    if map_differs(shown):
        raise AssertionError(f"the map differs from the Pack at {box(map_differs(shown))}")


def an_overlay_section_that_lies_is_left_out() -> None:
    """OVLY's structure wrong behind a right CRC (an item of 300 points
    where there are 2): the reader checks it byte by byte, and leaves it
    out."""
    data = bytearray(TRANSIT.read_bytes())
    i, offset, length = ovly(bytes(data))
    body = bytearray(data[offset:offset + length])
    name_len = body[2]
    first_item = 2 + 1 + name_len + 2
    struct.pack_into("<H", body, first_item + 2, 300)
    data[offset:offset + length] = body
    struct.pack_into("<I", data, 32 + 16 * i + 12, zlib.crc32(bytes(body)))
    count = struct.unpack_from("<H", data, 18)[0]
    struct.pack_into("<I", data, 28, zlib.crc32(bytes(data[:28]) + bytes(data[32:32 + 16 * count])))
    shown = run(bytes(data))
    need(shown.log, "(maps) the Pack's overlays are damaged: left out")
    if map_differs(shown):
        raise AssertionError(f"the map differs from the Pack at {box(map_differs(shown))}")


CASES: tuple[tuple[str, Callable[[], None]], ...] = (
    ("the-network-is-drawn-over-the-map", the_network_is_drawn_over_the_map),
    ("the-lines-cross-the-view-with-the-stops-off", the_lines_cross_the_view_with_the_stops_off),
    ("the-network-switches-off-and-on", the_network_switches_off_and_on),
    ("a-tapped-stop-shows-its-lines", a_tapped_stop_shows_its_lines),
    ("a-tapped-line-shows-its-routes", a_tapped_line_shows_its_routes),
    ("damaged-overlays-leave-the-map", damaged_overlays_leave_the_map),
    ("an-overlay-section-that-lies-is-left-out", an_overlay_section_that_lies_is_left_out),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=test_maps.SIMULATOR)
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()
    test_maps.SIMULATOR = args.sim.resolve()
    test_map_overlays.test_maps.SIMULATOR = test_maps.SIMULATOR
    if not test_maps.SIMULATOR.is_file():
        parser.error(f"simulator not found: {test_maps.SIMULATOR}")

    cases = [c for c in CASES if not args.only or args.only in c[0]]
    print("=== Map transit network ===")
    failures = 0
    for name, case in cases:
        try:
            case()
        except (AssertionError, subprocess.TimeoutExpired) as exc:
            failures += 1
            print(f"FAIL  {name}: {exc}")
        else:
            print(f"PASS  {name}")
    if failures:
        print(f"{failures}/{len(cases)} map transit tests failed")
        return 1
    print(f"All {len(cases)} map transit tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
