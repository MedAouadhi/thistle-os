#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The Maps app on the simulated T-Deck Max: it opens the Map Pack on the
card and shows the map around a fixed position, the first Region's centre,
pixel for pixel as the Pack holds it, with the OpenStreetMap credit on its
About line.

The Pack is the Map Pack tool's golden fixture,
tools/maps/testdata/fixture.tmap, and it is read here by the tool tests'
own reader (tools/maps/test_thistle_maps.py), which shares no code with the
app. What the screen should show is worked out from that reader: the tiles
around the centre, and the hatching docs/maps/map-pack-format.md specifies
where the Pack has no tile.

The app logs what it opened, where it shows, where its map is on the
screen, and its credit:

    (maps) opened fixture.tmap: format 1.0, 2 regions, 22 tiles
    (maps) showing z16 around 36.8000000, 10.1800000 (Medina)
    (maps) view 240x296 at 0,24
    (maps) about: © OpenStreetMap contributors. ...

Every case runs on a scratch copy of simulator/sdcard.
"""

from __future__ import annotations

import argparse
import math
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

ROOT = Path(__file__).resolve().parents[2]
SDCARD = ROOT / "simulator" / "sdcard"
GOLDEN = ROOT / "tools" / "maps" / "testdata" / "fixture.tmap"

sys.path.insert(0, str(ROOT / "tools" / "maps"))
from test_thistle_maps import Pack, components, world_pixel  # noqa: E402

APP = "com.thistle.maps"
# The bottom of the view holds the About line, over the map.
ABOUT_H = 30


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    magic, width, height, depth, pixels = data.split(maxsplit=4)
    if magic != b"P6" or depth != b"255":
        raise AssertionError(f"{path} is not an 8-bit PPM")
    return int(width), int(height), pixels


def dark(shot, x: int, y: int) -> bool:
    width, _, pixels = shot
    return pixels[(y * width + x) * 3] < 128


def no_map_here(gx: int, gy: int) -> bool:
    """The hatching drawn where the Pack has no tile (the format document's
    "No map here")."""
    return (gx + gy) % 8 == 0 or (gx - gy) % 8 == 0


@dataclass
class Shown:
    log: str
    shot: Optional[tuple[int, int, bytes]]


def run_app(pack: Optional[bytes], name: str = "fixture.tmap", timeout_ms: int = 3000) -> Shown:
    with tempfile.TemporaryDirectory(prefix="thistle-maps-") as tmp:
        card = Path(tmp) / "card"
        shutil.copytree(SDCARD, card)
        maps = card / "maps"
        shutil.rmtree(maps, ignore_errors=True)
        maps.mkdir()
        if pack is not None:
            (maps / name).write_bytes(pack)
        shot_path = Path(tmp) / "shot.ppm"
        env = dict(__import__("os").environ, THISTLE_SIM_SDCARD=str(card))
        command = [str(SIMULATOR), "--headless", "--device", "tdeck-max",
                   "--launch-app", APP, "--timeout", str(timeout_ms),
                   "--screenshot", str(shot_path)]
        run = subprocess.run(command, text=True, capture_output=True, errors="replace",
                             check=False, timeout=timeout_ms / 1000 + 30, env=env)
        log = run.stdout + run.stderr
        if run.returncode != 0:
            tail = "\n".join(log.splitlines()[-20:])
            raise AssertionError(f"simulator exited {run.returncode}:\n{tail}")
        if f"App launched: {APP}" not in log:
            raise AssertionError("the app did not launch")
        shot = read_ppm(shot_path) if shot_path.exists() else None
    return Shown(log, shot)


def need(log: str, line: str) -> None:
    if line not in log:
        maps_lines = "\n".join(l for l in log.splitlines() if "(maps)" in l)
        raise AssertionError(f"no {line!r}; the app said:\n{maps_lines}")


def view_rect(log: str) -> tuple[int, int, int, int]:
    found = re.search(r"\(maps\) view (\d+)x(\d+) at (\d+),(\d+)", log)
    if not found:
        raise AssertionError("the app did not say where its view is")
    w, h, x, y = (int(v) for v in found.groups())
    return x, y, w, h


def expected_origin(pack: Pack, z: int, w: int, h: int) -> tuple[int, int]:
    """The world pixel at the view's top left: the first Region's centre in
    the middle of the view, as the format document says."""
    lat, lon = pack.regions()[0]["centre"]
    gx, gy = world_pixel(lat, lon, z)
    return math.floor(gx + 0.5) - w // 2, math.floor(gy + 0.5) - h // 2


# --------------------------------------------------------------------------
# Cases
# --------------------------------------------------------------------------

def opens_the_pack_on_the_first_region() -> None:
    shown = run_app(GOLDEN.read_bytes())
    need(shown.log, "(maps) opened fixture.tmap: format 1.0, 2 regions, 22 tiles")
    need(shown.log, "(maps) showing z16 around 36.8000000, 10.1800000 (Medina)")


def shows_the_packs_tiles_pixel_for_pixel() -> None:
    pack = Pack(GOLDEN.read_bytes())
    shown = run_app(GOLDEN.read_bytes())
    vx, vy, w, h = view_rect(shown.log)
    if (vx, w) != (0, 240):
        raise AssertionError(f"the view is {w} wide at x {vx}, not the screen's width")
    left, top = expected_origin(pack, 16, w, h)
    index = pack.index()
    tiles: dict = {}
    wrong = []
    hatched = 0
    for sy in range(h - ABOUT_H):
        for sx in range(w):
            gx, gy = left + sx, top + sy
            key = (16, gx // 256, gy // 256)
            if key in index:
                if key not in tiles:
                    tiles[key] = pack.tile(*key)
                bitmap = tiles[key]
                want = bool(bitmap[(gy % 256) * 32 + (gx % 256) // 8] & (0x80 >> (gx % 8)))
            else:
                hatched += 1
                want = no_map_here(gx, gy)
            if dark(shown.shot, vx + sx, vy + sy) != want:
                wrong.append((sx, sy))
    if wrong:
        raise AssertionError(f"{len(wrong)} pixels differ from the Pack, the first at "
                             f"{wrong[0]} of the view")
    if not tiles:
        raise AssertionError("no tile of the Pack is on the screen")


def arabic_is_shaped_and_joined_on_the_screen() -> None:
    """The quarter's name, محمد, has no dots: shaped, it is one connected
    shape on the screen; letter by letter it would be four."""
    pack = Pack(GOLDEN.read_bytes())
    shown = run_app(GOLDEN.read_bytes())
    vx, vy, w, h = view_rect(shown.log)
    left, top = expected_origin(pack, 16, w, h)
    lat = 36.8000 + 120 / 111_320.0
    lon = 10.1800 + 120 / (111_320.0 * math.cos(math.radians(36.8)))
    gx, gy = world_pixel(lat, lon, 16)
    cx, cy = int(gx) - left + vx, int(gy) - top + vy
    inked = {(x, y) for x in range(cx - 40, cx + 41) for y in range(cy - 14, cy + 15)
             if dark(shown.shot, x, y)}
    groups = [g for g in components(inked) if len(g) > 3]
    if len(groups) != 1:
        raise AssertionError(f"the name is {len(groups)} shapes on the screen, not one")


def the_credit_is_on_the_about_line() -> None:
    shown = run_app(GOLDEN.read_bytes())
    need(shown.log, "(maps) about: © OpenStreetMap contributors.")
    vx, vy, w, h = view_rect(shown.log)
    # The About line is text on paper: ink in it, and a rule above it.
    rows = range(vy + h - ABOUT_H + 4, vy + h)
    if not any(dark(shown.shot, x, y) for x in range(vx, vx + w) for y in rows):
        raise AssertionError("nothing is drawn on the About line")


def no_pack_says_where_to_put_one() -> None:
    shown = run_app(None)
    need(shown.log, "(maps) no Map Pack in /sdcard/maps")


def a_file_that_is_no_pack_is_refused() -> None:
    shown = run_app(b"this is not a map pack at all" * 4, name="broken.tmap")
    need(shown.log, "(maps) cannot open broken.tmap: not a Map Pack")


def a_newer_format_is_refused_with_a_message() -> None:
    data = bytearray(GOLDEN.read_bytes())
    data[8] = 2  # the major version
    shown = run_app(bytes(data), name="newer.tmap")
    need(shown.log, "(maps) cannot open newer.tmap: made by a newer Map Pack tool (format 2)")


CASES: tuple[tuple[str, Callable[[], None]], ...] = (
    ("opens-the-pack-on-the-first-region", opens_the_pack_on_the_first_region),
    ("shows-the-packs-tiles-pixel-for-pixel", shows_the_packs_tiles_pixel_for_pixel),
    ("arabic-is-shaped-and-joined-on-the-screen", arabic_is_shaped_and_joined_on_the_screen),
    ("the-credit-is-on-the-about-line", the_credit_is_on_the_about_line),
    ("no-pack-says-where-to-put-one", no_pack_says_where_to_put_one),
    ("a-file-that-is-no-pack-is-refused", a_file_that_is_no_pack_is_refused),
    ("a-newer-format-is-refused-with-a-message", a_newer_format_is_refused_with_a_message),
)

SIMULATOR = Path("build/thistle_sim")


def main() -> int:
    global SIMULATOR
    parser = argparse.ArgumentParser()
    parser.add_argument("--sim", type=Path, default=SIMULATOR)
    parser.add_argument("--only", help="run only the cases whose name holds this")
    args = parser.parse_args()
    SIMULATOR = args.sim.resolve()
    if not SIMULATOR.is_file():
        parser.error(f"simulator not found: {SIMULATOR}")

    cases = [c for c in CASES if not args.only or args.only in c[0]]
    print("=== Maps ===")
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
        print(f"{failures}/{len(cases)} Maps tests failed")
        return 1
    print(f"All {len(cases)} Maps tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
