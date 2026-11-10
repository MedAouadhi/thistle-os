#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The Transit app on the simulated T-Deck Max, against a fake Berlin
Provider server (fake_transit_provider.py, vbb-rest 6's shapes), as
test_hermes.py runs Hermes against a fake bridge.

The app shows its own map of the card's Map Pack (the golden fixture, as in
test_maps.py) with the Provider's stops and vehicles as overlays: it fetches
at once when it opens, then every interval, on its own task, only while it
is open. A changed overlay is drawn at most every 20 s and then only where
it changed; Refresh fetches and draws at once; a tap on a stop shows its
departures.

The card's config/transit.json points the Provider at the fake server and,
for the live cases, shortens its interval to 5 s. Positions are worked out
from the view's origin as in test_map_overlays.py.

The app logs what it does:

    (transit) view 240x296 at 0,24
    (transit) berlin: fetched 2 stops, 1 vehicles, 0 departures in 3 ms, for the 5 s interval
    (transit) stops: 2; vehicles: 1
    (transit) card: Alexanderplatz; 16:47 M10 S+U Hauptbahnhof +2; 16:52 200 Zoo
    (transit) berlin: stopped fetching
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
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_maps  # noqa: E402
from test_maps import GOLDEN, SDCARD, Pack, dark, read_ppm  # noqa: E402
from test_map_overlays import REDRAWN, flushes_after  # noqa: E402
from fake_transit_provider import Departure, FakeTransit, Stop, Vehicle  # noqa: E402

APP = "com.thistle.transit"
Z = 16


@dataclass
class Run:
    log: str
    shot: Optional[tuple[int, int, bytes]]
    fake: FakeTransit
    files: dict[str, Optional[str]]


def lat_lon_of(gx: float, gy: float, z: int) -> tuple[float, float]:
    size = 256.0 * (1 << z)
    lon = gx / size * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * gy / size))))
    return lat, lon


_VIEW: Optional[tuple[int, int, int, int, int, int]] = None


def opening_view() -> tuple[int, int, int, int, int, int]:
    """(vx, vy, w, h, left, top): the view on the screen and its origin as
    the app opens, on the first Region's centre at z16."""
    global _VIEW
    if _VIEW is None:
        probe = run_app(FakeTransit(), timeout_ms=1500)
        found = re.search(r"\(transit\) view (\d+)x(\d+) at (\d+),(\d+)", probe.log)
        if not found:
            raise AssertionError(f"the app did not say where its view is:\n{said(probe.log)}")
        w, h, vx, vy = (int(v) for v in found.groups())
        left, top = test_maps.expected_origin(Pack(GOLDEN.read_bytes()), Z, w, h)
        _VIEW = (vx, vy, w, h, left, top)
    return _VIEW


def at_screen(sx: int, sy: int) -> tuple[float, float]:
    """(lat, lon) of screen pixel (sx, sy) in the opening view."""
    vx, vy, w, h, left, top = opening_view()
    return lat_lon_of(left + sx - vx + 0.5, top + sy - vy + 0.5, Z)


def stop(id_: str, name: str, sx: int, sy: int) -> Stop:
    return Stop(id_, name, *at_screen(sx, sy))


def vehicle(line: str, direction: str, sx: int, sy: int) -> Vehicle:
    return Vehicle(line, direction, *at_screen(sx, sy))


def said(log: str) -> str:
    return "\n".join(l for l in log.splitlines() if "(transit)" in l or "(map_view)" in l)


def need(log: str, line: str) -> None:
    if line not in log:
        raise AssertionError(f"no {line!r}; the app said:\n{said(log)}")


def run_app(fake: FakeTransit, timeout_ms: int = 4000, steps: tuple[str, ...] = (),
            config: Optional[dict] = None, app: str = APP,
            read_back: tuple[str, ...] = ()) -> Run:
    """Runs app on a scratch card holding the golden Pack, its
    config/transit.json naming the fake server (with config's keys over
    it). Steps as test_maps.run_app's. read_back: card files to return."""
    fake.start()
    try:
        with tempfile.TemporaryDirectory(prefix="thistle-transit-") as tmp:
            card = Path(tmp) / "card"
            shutil.copytree(SDCARD, card)
            maps = card / "maps"
            shutil.rmtree(maps, ignore_errors=True)
            maps.mkdir()
            (maps / "fixture.tmap").write_bytes(GOLDEN.read_bytes())
            settings = {"provider": "berlin", "base_url": fake.url}
            settings.update(config or {})
            (card / "config" / "transit.json").write_text(json.dumps(settings))
            shot_path = Path(tmp) / "shot.ppm"
            env = dict(os.environ, THISTLE_SIM_SDCARD=str(card))
            command = [str(test_maps.SIMULATOR), "--headless", "--device", "tdeck-max",
                       "--launch-app", app, "--timeout", str(timeout_ms),
                       "--screenshot", str(shot_path), "--log-flushes"]
            for step in steps:
                command += ["--drag" if step.count(",") == 3 else "--tap", step]
            result = subprocess.run(command, text=True, capture_output=True, errors="replace",
                                    check=False, timeout=timeout_ms / 1000 + 30, env=env)
            log = result.stdout + result.stderr
            if result.returncode != 0:
                tail = "\n".join(log.splitlines()[-20:])
                raise AssertionError(f"simulator exited {result.returncode}:\n{tail}")
            if f"App launched: {app}" not in log:
                raise AssertionError(f"{app} did not launch")
            shot = read_ppm(shot_path) if shot_path.exists() else None
            files = {p: (card / p).read_text() if (card / p).exists() else None
                     for p in read_back}
    finally:
        fake.stop()
    return Run(log, shot, fake, files)


def logged_at(log: str, what: str) -> tuple[int, int]:
    found = re.search(rf"\(transit\) {re.escape(what)} at (\d+),(\d+)", log)
    if not found:
        raise AssertionError(f"the app did not say where {what} is:\n{said(log)}")
    return int(found.group(1)), int(found.group(2))


def near_dark(shot, x: int, y: int, r: int = 2) -> bool:
    return any(dark(shot, x + dx, y + dy) for dx in range(-r, r + 1) for dy in range(-r, r + 1))


# Where the cases put things: clear of the buttons in the corners.
STOP_A = (150, 150)
STOP_B = (80, 230)
BUS = (100, 100)
BUS_MOVED = (100, 140)


def network() -> tuple[list[Stop], list[Vehicle]]:
    return ([stop("900100003", "Alexanderplatz", *STOP_A),
             stop("900100005", "Rotes Rathaus", *STOP_B)],
            [vehicle("M10", "S+U Hauptbahnhof", *BUS)])


# --------------------------------------------------------------------------
# Cases
# --------------------------------------------------------------------------

def stops_and_vehicles_are_drawn() -> None:
    """At open, one fetch for the map's box: the stops and the vehicles are
    drawn at once, on the Pack's map."""
    stops, vehicles = network()
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles]))
    need(run.log, "(transit) berlin: fetched 2 stops, 1 vehicles, 0 departures")
    need(run.log, "(transit) stops: 2; vehicles: 1")
    need(run.log, "(map_view) overlays redrawn for")
    for name, (x, y) in (("stop", STOP_A), ("stop", STOP_B), ("vehicle", BUS)):
        if not near_dark(run.shot, x, y):
            raise AssertionError(f"no {name} drawn at {x},{y}")
    # The radar's box holds the whole view.
    radar = run.fake.asked("/radar")
    if len(radar) != 1:
        raise AssertionError(f"{len(radar)} radar requests at open, not one")
    q = {k: float(v) for k, v in radar[0].query.items() if k in ("north", "south", "east", "west")}
    vx, vy, w, h, _, _ = opening_view()
    top_left, bottom_right = at_screen(vx, vy), at_screen(vx + w - 1, vy + h - 1)
    if not (q["north"] >= top_left[0] and q["west"] <= top_left[1] and
            q["south"] <= bottom_right[0] and q["east"] >= bottom_right[1]):
        raise AssertionError(f"the radar box {q} does not hold the view")
    nearby = run.fake.asked("/locations/nearby")
    if len(nearby) != 1:
        raise AssertionError(f"{len(nearby)} stop requests at open, not one")


def a_moved_vehicle_waits_20_s_and_refreshes_only_what_changed() -> None:
    """Fetched every 5 s, the vehicle moves at the second fetch: the view
    draws it 20 s after its last draw, refreshing only around it."""
    stops, vehicles = network()
    moved = [vehicle("M10", "S+U Hauptbahnhof", *BUS_MOVED)]
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles, moved]), timeout_ms=26000,
                  config={"min_interval_s": 5})
    need(run.log, "for the 5 s interval")
    found = None
    for m in REDRAWN.finditer(run.log):
        if m.group(1) == "a change":
            found = m
            break
    if not found:
        raise AssertionError(f"the move was never drawn:\n{said(run.log)}")
    gap = float(found.group(2))
    x1, y1, x2, y2 = (int(v) for v in found.groups()[2:])
    if gap < 20.0:
        raise AssertionError(f"the move was drawn {gap} s after the last draw, under 20 s")
    bx, by = BUS
    mx, my = BUS_MOVED
    if not (x1 <= bx and x2 >= bx and y1 <= by and y2 >= my):
        raise AssertionError(f"the changed rectangle {x1},{y1}-{x2},{y2} misses the vehicle")
    if (x2 - x1 + 1) * (y2 - y1 + 1) > 120 * 100:
        raise AssertionError(f"the changed rectangle {x1},{y1}-{x2},{y2} is not small")
    flushes, refreshes = flushes_after(run.log, found.group(0))
    for fx1, fy1, fx2, fy2 in flushes:
        if fx1 < x1 - 1 or fy1 < y1 - 1 or fx2 > x2 + 1 or fy2 > y2 + 1:
            raise AssertionError(f"a flush of {fx1},{fy1}-{fx2},{fy2} reaches past the "
                                 f"changed rectangle {x1},{y1}-{x2},{y2}")
    if refreshes != 1:
        raise AssertionError(f"the move cost {refreshes} refreshes, not one")
    if not near_dark(run.shot, *BUS_MOVED):
        raise AssertionError("the vehicle is not drawn where it went")
    if len(run.fake.asked("/radar")) < 4:
        raise AssertionError("the app did not fetch every 5 s")


def refresh_fetches_and_draws_at_once() -> None:
    """With the Provider's own 30 s interval, Refresh fetches at once and
    the view draws what came back at once."""
    stops, vehicles = network()
    moved = [vehicle("M10", "S+U Hauptbahnhof", *BUS_MOVED)]
    probe = run_app(FakeTransit(stops=stops, vehicles=[vehicles]), timeout_ms=2000)
    refresh = logged_at(probe.log, "refresh")
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles, moved]), timeout_ms=10000,
                  steps=("wait", "wait", "wait", f"{refresh[0]},{refresh[1]}"))
    need(run.log, "(transit) refresh")
    need(run.log, "for Refresh")
    radar = run.fake.asked("/radar")
    if len(radar) != 2:
        raise AssertionError(f"{len(radar)} radar requests, not two (open, Refresh)")
    found = [m for m in REDRAWN.finditer(run.log) if m.group(1) == "Refresh"]
    if len(found) < 2:
        raise AssertionError(f"Refresh's result was not drawn at once:\n{said(run.log)}")
    if not near_dark(run.shot, *BUS_MOVED):
        raise AssertionError("the vehicle is not drawn where Refresh found it")


def a_tapped_stop_shows_its_departures() -> None:
    stops, vehicles = network()
    departures = {"900100003": [
        Departure("2026-11-10T16:47:00+01:00", "2026-11-10T16:45:00+01:00", 120, "M10",
                  "S+U Hauptbahnhof"),
        Departure(None, "2026-11-10T16:52:00+01:00", None, "200", "Zoo"),
    ]}
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles], departures=departures),
                  steps=(f"{STOP_A[0] + 6},{STOP_A[1] + 5}",), timeout_ms=5000)
    need(run.log, "(transit) card: Alexanderplatz; asking for its departures")
    asked = run.fake.asked("/stops/")
    if not asked or asked[0].path != "/stops/900100003/departures":
        raise AssertionError(f"departures asked as {[r.path for r in asked]}")
    need(run.log, "for a stop tapped")
    need(run.log, "(transit) card: Alexanderplatz; 16:47 M10 S+U Hauptbahnhof +2; "
                  "16:52 200 Zoo")
    card = logged_at(run.log, "card shown")
    if not dark(run.shot, *card) and not near_dark(run.shot, *card, r=20):
        raise AssertionError("no card is drawn")


def nothing_is_fetched_once_the_app_is_closed() -> None:
    """Fetched every 5 s while open; Home closes it at 3.75 s, and nothing
    is asked after, though the run goes on to 14 s."""
    stops, vehicles = network()
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles]), timeout_ms=14000,
                  steps=("wait", "home"), config={"min_interval_s": 5})
    need(run.log, "(transit) berlin: stopped fetching")
    radar = run.fake.asked("/radar")
    if len(radar) != 1:
        raise AssertionError(f"{len(radar)} radar requests; the app fetched after it closed")
    if len(run.fake.requests) != 2:
        raise AssertionError(f"{[r.path for r in run.fake.requests]} asked; only the opening "
                             f"fetch should be")


def nothing_is_fetched_while_the_app_was_never_opened() -> None:
    stops, vehicles = network()
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles]), timeout_ms=8000,
                  app="com.thistle.todo", config={"min_interval_s": 5})
    if run.fake.requests:
        raise AssertionError(f"{[r.path for r in run.fake.requests]} asked with Transit closed")
    if "(transit) berlin: fetching" in run.log:
        raise AssertionError("the fetch task started with Transit closed")


def a_failed_fetch_keeps_the_last_result_and_says_why() -> None:
    stops, vehicles = network()
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles], fail_radar=(2,)),
                  timeout_ms=9000, config={"min_interval_s": 5})
    need(run.log, "(transit) berlin: fetch failed: HTTP 500, Transit service not answering; "
                  "trying again in 5 s")
    need(run.log, "(transit) status: Transit service not answering")
    if not near_dark(run.shot, *BUS):
        raise AssertionError("the vehicle went with the failed fetch")


def the_last_result_is_saved_as_geojson() -> None:
    stops, vehicles = network()
    path = "maps/overlays/transit-berlin.geojson"
    run = run_app(FakeTransit(stops=stops, vehicles=[vehicles]), timeout_ms=3000,
                  config={"save_geojson": True}, read_back=(path,))
    need(run.log, "(transit) saved the last result: 1 vehicles, 2 stops in " + path)
    text = run.files[path]
    if text is None:
        raise AssertionError(f"{path} was not written")
    data = json.loads(text)
    names = sorted(f["properties"]["name"] for f in data["features"])
    if data["type"] != "FeatureCollection" or names != ["Alexanderplatz", "M10",
                                                        "Rotes Rathaus"]:
        raise AssertionError(f"{path} holds {names}")


CASES: tuple[tuple[str, Callable[[], None]], ...] = (
    ("stops-and-vehicles-are-drawn", stops_and_vehicles_are_drawn),
    ("a-moved-vehicle-waits-20-s-and-refreshes-only-what-changed",
     a_moved_vehicle_waits_20_s_and_refreshes_only_what_changed),
    ("refresh-fetches-and-draws-at-once", refresh_fetches_and_draws_at_once),
    ("a-tapped-stop-shows-its-departures", a_tapped_stop_shows_its_departures),
    ("nothing-is-fetched-once-the-app-is-closed", nothing_is_fetched_once_the_app_is_closed),
    ("nothing-is-fetched-while-the-app-was-never-opened",
     nothing_is_fetched_while_the_app_was_never_opened),
    ("a-failed-fetch-keeps-the-last-result-and-says-why",
     a_failed_fetch_keeps_the_last_result_and_says_why),
    ("the-last-result-is-saved-as-geojson", the_last_result_is_saved_as_geojson),
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
    print("=== Transit ===")
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
        print(f"{failures}/{len(cases)} transit tests failed")
        return 1
    print(f"All {len(cases)} transit tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
