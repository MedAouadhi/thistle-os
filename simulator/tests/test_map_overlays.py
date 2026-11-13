#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Overlays on the Maps app's map, on the simulated T-Deck Max: every
GeoJSON file in /sdcard/maps/overlays/ is a switchable Overlay, drawn over
the Map Pack's tiles, decluttered, at most 256 items, redrawn after a change
at most every 20 s and then only where it changed, at once on Refresh; a tap
near an item shows its info card.

The Pack is the golden fixture, as in test_maps.py, whose helpers this
reuses; what the screen should show where no overlay draws is the Pack's
tiles, read by the tool tests' own reader. Positions are worked out here
from the view's origin, which the app logs, and Web Mercator.

The app and the view log what they do:

    (maps) overlay pois.geojson: 3 items
    (maps) overlay many.geojson: 256 items, 44 left out (256 at most)
    (map_view) overlays drawn: 1 on, 3 items in view, 0 left out to declutter
    (map_view) overlay "live" changed; drawn in 13.9 s, 20 s after the last
    (map_view) overlays redrawn for a change, 20.0 s after the last: changed 100,140-130,160
    (map_view) tap at 120,172: "pois" item 0, 3 px away
    (maps) info card: Fountain; kind: water; depth: 2
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_maps  # noqa: E402
from test_maps import (GOLDEN, SDCARD, APP, FLUSH, REFRESH, Pack, dark,  # noqa: E402
                       expected_origin, need, read_ppm, view_rect, world_pixel)

STATUS_BAR_H = 24
ABOUT_H = 30
Z = 16  # the app opens the fixture at z16


@dataclass
class Run:
    log: str
    shot: Optional[tuple[int, int, bytes]]


def lat_lon_of(gx: float, gy: float, z: int) -> tuple[float, float]:
    size = 256.0 * (1 << z)
    lon = gx / size * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * gy / size))))
    return lat, lon


def opening_view() -> tuple[int, int, int, int, int, int]:
    """The view's place on the screen, and its origin when the app opens:
    (vx, vy, w, h, left, top). Asked of the app once."""
    global _VIEW
    if _VIEW is None:
        probe = test_maps.run_app(GOLDEN.read_bytes(), timeout_ms=1500)
        vx, vy, w, h = view_rect(probe.log)
        left, top = expected_origin(Pack(GOLDEN.read_bytes()), Z, w, h)
        _VIEW = (vx, vy, w, h, left, top)
    return _VIEW


_VIEW: Optional[tuple[int, int, int, int, int, int]] = None


def at_screen(sx: int, sy: int) -> list[float]:
    """GeoJSON's [lon, lat] of screen pixel (sx, sy) in the opening view,
    in the middle of the pixel."""
    vx, vy, w, h, left, top = opening_view()
    lat, lon = lat_lon_of(left + sx - vx + 0.5, top + sy - vy + 0.5, Z)
    return [lon, lat]


def point(sx: int, sy: int, **props) -> dict:
    return {"type": "Feature", "properties": props,
            "geometry": {"type": "Point", "coordinates": at_screen(sx, sy)}}


def collection(*features: dict) -> bytes:
    return json.dumps({"type": "FeatureCollection", "features": list(features)},
                      ensure_ascii=False).encode()


def run_app(overlays: dict[str, bytes], timeout_ms: int = 3500, steps: tuple[str, ...] = (),
            during: Optional[Callable[[Path], None]] = None) -> Run:
    """Runs the app on a scratch card holding the golden Pack and overlays
    (file name to contents) in maps/overlays/. Steps as test_maps.run_app's.
    during, if given, is called with the overlays folder while it runs."""
    with tempfile.TemporaryDirectory(prefix="thistle-overlays-") as tmp:
        card = Path(tmp) / "card"
        shutil.copytree(SDCARD, card)
        maps = card / "maps"
        shutil.rmtree(maps, ignore_errors=True)
        (maps / "overlays").mkdir(parents=True)
        (maps / "fixture.tmap").write_bytes(GOLDEN.read_bytes())
        for name, data in overlays.items():
            (maps / "overlays" / name).write_bytes(data)
        shot_path = Path(tmp) / "shot.ppm"
        env = dict(os.environ, THISTLE_SIM_SDCARD=str(card))
        command = [str(test_maps.SIMULATOR), "--headless", "--device", "tdeck-max",
                   "--launch-app", APP, "--timeout", str(timeout_ms),
                   "--screenshot", str(shot_path), "--log-flushes"]
        for step in steps:
            command += ["--drag" if step.count(",") == 3 else "--tap", step]
        with tempfile.TemporaryFile("w+", errors="replace") as out:
            proc = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, text=True,
                                    env=env)
            try:
                if during:
                    during(maps / "overlays")
                proc.wait(timeout=timeout_ms / 1000 + 30)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
            out.seek(0)
            log = out.read()
        if proc.returncode != 0:
            tail = "\n".join(log.splitlines()[-20:])
            raise AssertionError(f"simulator exited {proc.returncode}:\n{tail}")
        if f"App launched: {APP}" not in log:
            raise AssertionError("the app did not launch")
        shot = read_ppm(shot_path) if shot_path.exists() else None
    return Run(log, shot)


def log_lines(log: str, tag: str) -> str:
    return "\n".join(l for l in log.splitlines() if tag in l)


def map_differs(run: Run) -> set[tuple[int, int]]:
    """The screen pixels of the view (but the About line and the buttons'
    corners) that are not the Pack's map: what overlays and cards drew."""
    pack = Pack(GOLDEN.read_bytes())
    vx, vy, w, h, left, top = opening_view()
    index = pack.index()
    tiles: dict = {}
    differ = set()
    for sy in range(h - ABOUT_H):
        for sx in range(w):
            if (sx >= w - 48 or sx < 48) and (sy < 48 or sy >= h - ABOUT_H - 90):
                continue  # the buttons in the corners
            gx, gy = left + sx, top + sy
            key = (Z, gx // 256, gy // 256)
            if key in index:
                if key not in tiles:
                    tiles[key] = pack.tile(*key)
                want = bool(tiles[key][(gy % 256) * 32 + (gx % 256) // 8] & (0x80 >> (gx % 8)))
            else:
                want = test_maps.no_map_here(gx, gy)
            if dark(run.shot, vx + sx, vy + sy) != want:
                differ.add((vx + sx, vy + sy))
    return differ


def box(pixels: set[tuple[int, int]]) -> tuple[int, int, int, int]:
    xs = [x for x, _ in pixels]
    ys = [y for _, y in pixels]
    return min(xs), min(ys), max(xs), max(ys)


def logged_at(log: str, what: str) -> tuple[int, int]:
    found = re.search(rf"\(maps\) {re.escape(what)} at (\d+),(\d+)", log)
    if not found:
        raise AssertionError(f"the app did not say where {what} is:\n{log_lines(log, '(maps)')}")
    return int(found.group(1)), int(found.group(2))


def tap(xy: tuple[int, int]) -> str:
    return f"{xy[0]},{xy[1]}"


# The screen point the cases put things at: right of the middle, clear of
# the buttons.
PX, PY = 150, 150


# --------------------------------------------------------------------------
# Cases
# --------------------------------------------------------------------------

def a_geojson_point_is_drawn_with_its_label() -> None:
    run = run_app({"pois.geojson": collection(point(PX, PY, name="Fountain"))})
    need(run.log, "(maps) overlay pois.geojson: 1 items")
    need(run.log, "(map_view) overlays drawn: 1 on, 1 items in view, 0 left out to declutter")
    if not dark(run.shot, PX, PY):
        raise AssertionError("no icon at the point")
    differ = map_differs(run)
    if not differ:
        raise AssertionError("the overlay drew nothing")
    x1, y1, x2, y2 = box(differ)
    # The icon around the point, its label to the right of it, nothing else.
    if x1 < PX - 9 or y1 < PY - 12 or y2 > PY + 12 or x2 > PX + 90:
        raise AssertionError(f"the overlay drew in {x1},{y1}-{x2},{y2}, not around "
                             f"{PX},{PY} and to its right")
    if x2 < PX + 30:
        raise AssertionError(f"no label right of the icon: it drew in {x1},{y1}-{x2},{y2}")


def lines_and_polygons_are_drawn() -> None:
    line = {"type": "Feature", "properties": {"name": "Path"},
            "geometry": {"type": "LineString",
                         "coordinates": [at_screen(60, 100), at_screen(180, 100)]}}
    area = {"type": "Feature", "properties": {"name": "Park"},
            "geometry": {"type": "Polygon", "coordinates": [[
                at_screen(80, 180), at_screen(160, 180), at_screen(160, 240),
                at_screen(80, 240), at_screen(80, 180)]]}}
    run = run_app({"shapes.geojson": collection(line, area)})
    need(run.log, "(maps) overlay shapes.geojson: 2 items")
    for x in range(70, 171, 10):
        if not dark(run.shot, x, 100):
            raise AssertionError(f"the line is not drawn at {x},100")
    for x, y in ((80, 210), (160, 210), (120, 180), (120, 240)):
        if not dark(run.shot, x, y):
            raise AssertionError(f"the polygon's outline is not drawn at {x},{y}")


def an_overlay_switches_off_and_on() -> None:
    """Off from the Layers panel, the map is the Pack's again, pixel for
    pixel; on again, the point is back."""
    overlays = {"pois.geojson": collection(point(PX, PY, name="Fountain"))}
    probe = run_app(overlays, steps=("wait",), timeout_ms=4500)
    layers = logged_at(probe.log, "layers")
    # Open the panel, then read where its row is.
    probe = run_app(overlays, steps=(tap(layers),), timeout_ms=4500)
    row = logged_at(probe.log, "layer pois")
    off = run_app(overlays, steps=(tap(layers), tap(row), tap(layers)), timeout_ms=7500)
    need(off.log, "(maps) overlay pois off")
    need(off.log, "(map_view) overlays drawn: 0 on")
    if map_differs(off):
        raise AssertionError(f"with the overlay off, the map differs from the Pack at "
                             f"{box(map_differs(off))}")
    on = run_app(overlays, steps=(tap(layers), tap(row), tap(row), tap(layers)),
                 timeout_ms=9000)
    need(on.log, "(maps) overlay pois on")
    if not dark(on.shot, PX, PY):
        raise AssertionError("switched on again, the point is not drawn")


def every_geojson_file_is_an_overlay() -> None:
    run = run_app({"a.geojson": collection(point(PX, PY, name="A")),
                   "b.json": collection(point(PX, PY + 60, name="B")),
                   "notes.txt": b"not an overlay",
                   "broken.geojson": b"{\"type\": \"FeatureCollection\", \"features\": [",
                   })
    need(run.log, "(maps) overlay a.geojson: 1 items")
    need(run.log, "(maps) overlay b.json: 1 items")
    need(run.log, "(maps) overlay broken.geojson: cannot read it: not GeoJSON")
    if "notes.txt" in run.log:
        raise AssertionError("a .txt file was taken for an overlay")
    if not (dark(run.shot, PX, PY) and dark(run.shot, PX, PY + 60)):
        raise AssertionError("both overlays' points are not drawn")


def points_on_top_of_each_other_are_decluttered() -> None:
    """Five points on one spot and one apart: two are drawn, and the screen
    shows one icon where the five are."""
    same = [point(PX, PY, name=f"Stop {i}") for i in range(5)]
    apart = point(PX - 60, PY + 80, name="Far")
    run = run_app({"crowd.geojson": collection(*same, apart)})
    need(run.log, "(map_view) overlays drawn: 1 on, 6 items in view, 4 left out to declutter")
    # One label, the first's: what is drawn right of the five is one line.
    differ = {p for p in map_differs(run) if p[1] < PY + 40}
    x1, y1, x2, y2 = box(differ)
    if y2 - y1 > 24:
        raise AssertionError(f"the crowd drew {y2 - y1 + 1} px high, more than one label")
    if not dark(run.shot, PX - 60, PY + 80):
        raise AssertionError("the point apart was decluttered away")


def an_overlay_holds_at_most_256_items() -> None:
    features = [point(10 + (i % 20) * 11, 40 + (i // 20) * 16) for i in range(300)]
    run = run_app({"many.geojson": collection(*features)})
    need(run.log, "(maps) overlay many.geojson: 256 items, 44 left out (256 at most)")
    found = re.search(r"\(map_view\) overlays drawn: 1 on, (\d+) items in view", run.log)
    if not found or int(found.group(1)) > 256:
        raise AssertionError(f"the view drew {found.group(1) if found else 'no'} items")


# The live cases: the file changes while the app runs.

REDRAWN = re.compile(r"\(map_view\) overlays redrawn for (a change|Refresh), ([\d.]+) s after "
                     r"the last: changed (\d+),(\d+)-(\d+),(\d+)")


def move_point_after(seconds: float, to: tuple[int, int]) -> Callable[[Path], None]:
    def during(folder: Path) -> None:
        time.sleep(seconds)
        data = collection(point(*to, name="Bus"))
        tmp = folder / "live.tmp"
        tmp.write_bytes(data)
        tmp.replace(folder / "live.geojson")
    return during


def flushes_after(log: str, marker: str) -> tuple[list[tuple[int, int, int, int]], int]:
    """The map's flushes after the line holding marker, up to the refresh
    that shows them, and how many refreshes showed something of the map
    from there to the end of the run: the status bar's clock refreshes on
    its own."""
    lines = log.splitlines()
    start = next(i for i, l in enumerate(lines) if marker in l)
    flushes, refreshes, map_drawn = [], 0, False
    for line in lines[start + 1:]:
        if (found := FLUSH.search(line)):
            area = tuple(int(v) for v in found.groups())
            if area[3] >= STATUS_BAR_H:
                map_drawn = True
                if refreshes == 0:
                    flushes.append(area)
        elif REFRESH in line and map_drawn:
            refreshes += 1
            map_drawn = False
    return flushes, refreshes


def a_changed_overlay_waits_20_s_and_refreshes_only_what_changed() -> None:
    first = collection(point(PX, PY, name="Bus"))
    run = run_app({"live.geojson": first}, timeout_ms=26000,
                  during=move_point_after(5.0, (PX, PY + 30)))
    need(run.log, '(map_view) overlay "live" changed; drawn in')
    found = REDRAWN.search(run.log)
    if not found:
        raise AssertionError(f"the change was never drawn:\n{log_lines(run.log, 'overlay')}")
    why, gap = found.group(1), float(found.group(2))
    x1, y1, x2, y2 = (int(v) for v in found.groups()[2:])
    if why != "a change" or gap < 20.0:
        raise AssertionError(f"the change was drawn for {why}, {gap} s after the last draw")
    # What changed is the icon and label where it was and where it is.
    if not (x1 <= PX - 5 and x2 >= PX + 5 and y1 <= PY - 5 and y2 >= PY + 35):
        raise AssertionError(f"the changed rectangle {x1},{y1}-{x2},{y2} misses the point")
    if (x2 - x1 + 1) * (y2 - y1 + 1) > 120 * 80:
        raise AssertionError(f"the changed rectangle {x1},{y1}-{x2},{y2} is not small")
    flushes, refreshes = flushes_after(run.log, found.group(0))
    if not flushes:
        raise AssertionError("nothing was flushed after the redraw")
    # LVGL grows an area it invalidates by a pixel.
    for fx1, fy1, fx2, fy2 in flushes:
        if fx1 < x1 - 1 or fy1 < y1 - 1 or fx2 > x2 + 1 or fy2 > y2 + 1:
            raise AssertionError(f"a flush of {fx1},{fy1}-{fx2},{fy2} reaches past the "
                                 f"changed rectangle {x1},{y1}-{x2},{y2}")
    if refreshes != 1:
        raise AssertionError(f"the redraw cost {refreshes} refreshes, not one")
    if not dark(run.shot, PX, PY + 30):
        raise AssertionError("the moved point is not drawn where it went")


def refresh_draws_a_change_at_once() -> None:
    first = collection(point(PX, PY, name="Bus"))
    probe = run_app({"live.geojson": first}, timeout_ms=2000)
    refresh = logged_at(probe.log, "refresh")
    # The file changes at 4 s; the app sees it within 2 s; Refresh at 9.75 s.
    steps = ("wait", "wait", "wait", "wait", "wait", tap(refresh))
    run = run_app({"live.geojson": first}, timeout_ms=12000, steps=steps,
                  during=move_point_after(4.0, (PX, PY + 30)))
    found = REDRAWN.search(run.log)
    if not found or found.group(1) != "Refresh":
        raise AssertionError(f"Refresh did not draw the change:\n{log_lines(run.log, 'overlay')}")
    if float(found.group(2)) >= 20.0:
        raise AssertionError("Refresh waited out the 20 s")
    if not dark(run.shot, PX, PY + 30):
        raise AssertionError("the moved point is not drawn where it went")


def a_tap_on_an_item_shows_its_info_card() -> None:
    overlays = {"pois.geojson": collection(
        point(PX, PY, name="Fountain", kind="water", depth=2),
        point(PX - 70, PY + 70, name="Bench"))}
    # 10 px from the Fountain: within 16 px.
    run = run_app(overlays, steps=(f"{PX - 8},{PY + 6}",), timeout_ms=4500)
    need(run.log, '"pois" item 0, 10 px away')
    need(run.log, "(maps) info card: Fountain; kind: water; depth: 2")
    differ = map_differs(run)
    if not any(y > PY + 40 and dark(run.shot, x, y) for x, y in differ):
        raise AssertionError("no card is drawn below the point")


def a_tap_far_from_every_item_selects_nothing() -> None:
    overlays = {"pois.geojson": collection(point(PX, PY, name="Fountain"))}
    run = run_app(overlays, steps=(f"{PX - 20},{PY + 12}",), timeout_ms=4500)
    need(run.log, "nothing within 16 px")
    if "(maps) info card" in run.log:
        raise AssertionError("a card was shown for a tap 23 px away")


def a_tap_on_the_card_closes_it() -> None:
    overlays = {"pois.geojson": collection(point(PX, PY, name="Fountain", kind="water"))}
    probe = run_app(overlays, steps=(f"{PX},{PY}",), timeout_ms=4500)
    card = logged_at(probe.log, "info card shown")
    run = run_app(overlays, steps=(f"{PX},{PY}", tap(card)), timeout_ms=6000)
    need(run.log, "(maps) info card closed")
    differ = {p for p in map_differs(run) if p[1] > PY + 40}
    if differ:
        raise AssertionError(f"the card is still drawn at {box(differ)}")


CASES: tuple[tuple[str, Callable[[], None]], ...] = (
    ("a-geojson-point-is-drawn-with-its-label", a_geojson_point_is_drawn_with_its_label),
    ("lines-and-polygons-are-drawn", lines_and_polygons_are_drawn),
    ("an-overlay-switches-off-and-on", an_overlay_switches_off_and_on),
    ("every-geojson-file-is-an-overlay", every_geojson_file_is_an_overlay),
    ("points-on-top-of-each-other-are-decluttered", points_on_top_of_each_other_are_decluttered),
    ("an-overlay-holds-at-most-256-items", an_overlay_holds_at_most_256_items),
    ("a-changed-overlay-waits-20-s-and-refreshes-only-what-changed",
     a_changed_overlay_waits_20_s_and_refreshes_only_what_changed),
    ("refresh-draws-a-change-at-once", refresh_draws_a_change_at_once),
    ("a-tap-on-an-item-shows-its-info-card", a_tap_on_an_item_shows_its_info_card),
    ("a-tap-far-from-every-item-selects-nothing", a_tap_far_from_every_item_selects_nothing),
    ("a-tap-on-the-card-closes-it", a_tap_on_the_card_closes_it),
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=test_maps.SIMULATOR)
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()
    test_maps.SIMULATOR = args.sim.resolve()
    if not test_maps.SIMULATOR.is_file():
        parser.error(f"simulator not found: {test_maps.SIMULATOR}")

    cases = [c for c in CASES if not args.only or args.only in c[0]]
    print("=== Map overlays ===")
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
        print(f"{failures}/{len(cases)} map overlay tests failed")
        return 1
    print(f"All {len(cases)} map overlay tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
