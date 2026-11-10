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
# The zoom buttons' half size, and some room for their border.
BUTTON_HALF = 21
STATUS_BAR_H = 24


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


def run_app(pack: Optional[bytes], name: str = "fixture.tmap", timeout_ms: int = 3000,
            steps: tuple[str, ...] = (), extra: tuple[str, ...] = ()) -> Shown:
    """Runs the app on a scratch card holding pack. Each step is a --tap
    (X,Y, or type:KEYS) or a --drag (X1,Y1,X2,Y2), 1.5 s apart, the first
    2.25 s after startup."""
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
                   "--screenshot", str(shot_path), "--log-flushes", *extra]
        for step in steps:
            command += ["--drag" if step.count(",") == 3 else "--tap", step]
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


def zoom_buttons(log: str) -> dict[str, tuple[int, int]]:
    found = dict(re.findall(r"\(maps\) zoom (in|out) at (\d+,\d+)", log))
    if set(found) != {"in", "out"}:
        raise AssertionError("the app did not say where its zoom buttons are")
    return {k: tuple(int(v) for v in xy.split(",")) for k, xy in found.items()}


def check_screen(shown: Shown, pack: Pack, z: int, left: int, top: int,
                 damaged: frozenset = frozenset()) -> int:
    """Every pixel of the view, but the About line and the zoom buttons,
    against the Pack's tiles and the hatching: a tile the Pack lacks, or one
    in damaged, is hatched. Returns how many of the Pack's tiles show."""
    vx, vy, w, h = view_rect(shown.log)
    if (vx, w) != (0, 240):
        raise AssertionError(f"the view is {w} wide at x {vx}, not the screen's width")
    buttons = zoom_buttons(shown.log)
    bx = min(x for x, _ in buttons.values()) - BUTTON_HALF
    by = min(y for _, y in buttons.values()) - BUTTON_HALF - vy
    index = pack.index()
    tiles: dict = {}
    wrong = []
    for sy in range(h - ABOUT_H):
        for sx in range(w):
            if sx >= bx and sy >= by:
                continue
            gx, gy = left + sx, top + sy
            key = (z, gx // 256, gy // 256)
            if key in index and key not in damaged:
                if key not in tiles:
                    tiles[key] = pack.tile(*key)
                bitmap = tiles[key]
                want = bool(bitmap[(gy % 256) * 32 + (gx % 256) // 8] & (0x80 >> (gx % 8)))
            else:
                want = no_map_here(gx, gy)
            if dark(shown.shot, vx + sx, vy + sy) != want:
                wrong.append((sx, sy))
    if wrong:
        raise AssertionError(f"{len(wrong)} pixels differ from the Pack at z{z} {left},{top}, "
                             f"the first at {wrong[0]} of the view")
    return len(tiles)


def shows_the_packs_tiles_pixel_for_pixel() -> None:
    pack = Pack(GOLDEN.read_bytes())
    shown = run_app(GOLDEN.read_bytes())
    vx, vy, w, h = view_rect(shown.log)
    left, top = expected_origin(pack, 16, w, h)
    if not check_screen(shown, pack, 16, left, top):
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


# --------------------------------------------------------------------------
# Moving: what each step drew, and how many refreshes it cost
# --------------------------------------------------------------------------

ACTION = re.compile(r"\(map_view\) (pan \w+|zoom in|zoom out|already at z\d+)")
SHOWN = re.compile(r"\(maps\) shown z(\d+) at (-?\d+),(-?\d+): (\d+) tiles, (\d+) places "
                   r"with none, (\d+) damaged")
FLUSH = re.compile(r"\[sim\] flush \((\d+),(\d+)\)-\((\d+),(\d+)\)")
REFRESH = "refreshing the e-paper panel"


@dataclass
class Step:
    action: str
    shown: list          # (z, left, top, tiles, missing, damaged) each view swapped in
    refreshes: int       # refreshes that showed something of the map
    early_flushes: int   # flushes of the map before its view was swapped in


def steps_of(log: str) -> list[Step]:
    """The log cut at each pan or zoom the view logged. A refresh counts for
    a step when what it shows includes the map: the status bar's clock may
    refresh on its own."""
    steps: list[Step] = []
    map_drawn = False
    for line in log.splitlines():
        if (found := ACTION.search(line)):
            steps.append(Step(found.group(1), [], 0, 0))
            continue
        if not steps:
            continue
        step = steps[-1]
        if (found := SHOWN.search(line)):
            step.shown.append(tuple(int(v) for v in found.groups()))
        elif (found := FLUSH.search(line)):
            if int(found.group(4)) >= STATUS_BAR_H:
                map_drawn = True
                if not step.shown:
                    step.early_flushes += 1
        elif REFRESH in line:
            if map_drawn:
                step.refreshes += 1
            map_drawn = False
    return steps


def first_shown(log: str) -> tuple[int, int, int]:
    found = SHOWN.search(log)
    if not found:
        raise AssertionError("the app never showed its map")
    z, left, top = (int(v) for v in found.groups()[:3])
    return z, left, top


def expect_steps(log: str, want: list[tuple[str, Optional[tuple[int, int, int]]]]) -> None:
    """Each step as wanted: its action, and the view it swapped in, with one
    refresh, drawn whole; or, for None, nothing drawn and no refresh."""
    steps = steps_of(log)
    got = [s.action for s in steps]
    if got != [a for a, _ in want]:
        raise AssertionError(f"the steps were {got}, not {[a for a, _ in want]}")
    for i, (step, (_, view)) in enumerate(zip(steps, want)):
        name = f"step {i + 1} ({step.action})"
        if view is None:
            if step.shown or step.refreshes:
                raise AssertionError(f"{name} drew: {step.shown}, {step.refreshes} refreshes")
            continue
        if [s[:3] for s in step.shown] != [view]:
            raise AssertionError(f"{name} showed {[s[:3] for s in step.shown]}, not {view}")
        if step.refreshes != 1:
            raise AssertionError(f"{name} cost {step.refreshes} refreshes, not one")
        if step.early_flushes:
            raise AssertionError(f"{name} drew the map {step.early_flushes} times before "
                                 f"its view was whole")


def pans_half_a_screen_a_step_with_one_refresh_each() -> None:
    """Swipes and keys, each a step of half the view: 120 px across, 148 px
    down. A swipe drags the map, so the view goes the other way."""
    pack = Pack(GOLDEN.read_bytes())
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=13000, steps=(
        "200,180,40,180",    # swipe left: east
        "120,280,120,100",   # swipe up: south
        "type:d",            # east
        "type:s",            # south
        "40,150,200,150",    # swipe right: west
        "type:w",            # north
        "type:a",            # west
    ))
    z, left, top = first_shown(shown.log)
    vx, vy, w, h = view_rect(shown.log)
    if (z, left, top) != (16, *expected_origin(pack, 16, w, h)):
        raise AssertionError(f"the map opened at z{z} {left},{top}")
    dx, dy = w // 2, h // 2
    moves = [("pan east", 1, 0), ("pan south", 0, 1), ("pan east", 1, 0), ("pan south", 0, 1),
             ("pan west", -1, 0), ("pan north", 0, -1), ("pan west", -1, 0)]
    want = []
    for action, mx, my in moves:
        left, top = left + mx * dx, top + my * dy
        want.append((action, (16, left, top)))
    expect_steps(shown.log, want)
    check_screen(shown, pack, 16, left, top)


def zooms_through_the_four_levels() -> None:
    """Out to z14 and in to z17, by the buttons and the keys, keeping the
    view's middle; past either end, nothing happens. One refresh a level."""
    pack = Pack(GOLDEN.read_bytes())
    for last, keys in ((14, ("out", "type:o", "type:o")),
                       (17, ("out", "type:o", "type:o", "type:i", "in", "type:i", "type:i",
                             "type:i"))):
        probe = run_app(GOLDEN.read_bytes(), timeout_ms=1000)
        buttons = zoom_buttons(probe.log)
        steps = tuple(f"{buttons[k][0]},{buttons[k][1]}" if k in buttons else k for k in keys)
        shown = run_app(GOLDEN.read_bytes(), timeout_ms=2500 + 1500 * len(steps), steps=steps)
        z, left, top = first_shown(shown.log)
        vx, vy, w, h = view_rect(shown.log)
        cx, cy = left + w // 2, top + h // 2
        want = []
        for key in keys:
            inward = key in ("in", "type:i")
            nz = z + (1 if inward else -1)
            if nz < 14 or nz > 17:
                want.append((f"already at z{z}", None))
                continue
            cx, cy = (cx * 2, cy * 2) if inward else (cx // 2, cy // 2)
            z = nz
            want.append(("zoom in" if inward else "zoom out", (z, cx - w // 2, cy - h // 2)))
        expect_steps(shown.log, want)
        if z != last:
            raise AssertionError(f"the case ends at z{z}, not z{last}")
        if not check_screen(shown, pack, z, cx - w // 2, cy - h // 2):
            raise AssertionError(f"no tile of the Pack shows at z{z}")


def outside_every_region_is_hatched() -> None:
    """Far enough east, the Pack has no tile: the view is all hatching."""
    pack = Pack(GOLDEN.read_bytes())
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=8000, steps=("type:d",) * 4)
    steps = steps_of(shown.log)
    z, left, top, tiles, missing, damaged = steps[-1].shown[-1]
    if tiles or damaged or not missing:
        raise AssertionError(f"the last view has {tiles} tiles, {damaged} damaged")
    if check_screen(shown, pack, z, left, top):
        raise AssertionError("a tile of the Pack shows outside every Region")


def tile_offset(data: bytes, key: tuple[int, int, int]) -> tuple[int, int]:
    """Where tile key's payload is in the Pack, by the format document."""
    count = int.from_bytes(data[18:20], "little")
    for i in range(count):
        entry = data[32 + 16 * i: 48 + 16 * i]
        if entry[:4] == b"TIDX":
            offset = int.from_bytes(entry[4:8], "little")
            length = int.from_bytes(entry[8:12], "little")
            for e in range(offset, offset + length, 24):
                x, y = (int.from_bytes(data[e + k: e + k + 4], "little") for k in (0, 4))
                if (data[e + 8], x, y) == key:
                    return (int.from_bytes(data[e + 12: e + 16], "little"),
                            int.from_bytes(data[e + 16: e + 20], "little"))
    raise AssertionError(f"no tile {key} in the Pack")


def a_bit_flipped_tile_is_hatched_and_the_rest_shown() -> None:
    pack = Pack(GOLDEN.read_bytes())
    data = bytearray(GOLDEN.read_bytes())
    bad = (16, 34620, 25553)
    offset, length = tile_offset(bytes(data), bad)
    data[offset + length // 2] ^= 0x10
    shown = run_app(bytes(data), name="flipped.tmap", timeout_ms=5000, steps=("type:d",))
    z, left, top = first_shown(shown.log)
    need(shown.log, f"(maps) shown z16 at {left},{top}: 3 tiles, 0 places with none, 1 damaged")
    vx, vy, w, h = view_rect(shown.log)
    # It goes on: a step east, the damaged tile still in view.
    check_screen(shown, pack, 16, left + w // 2, top, damaged=frozenset({bad}))


def a_truncated_pack_is_refused_and_the_app_goes_on() -> None:
    data = GOLDEN.read_bytes()
    for cut, why in ((len(data) // 2, "damaged"), (len(data) - 1, "damaged"),
                     (20, "not a Map Pack")):
        shown = run_app(data[:cut], name="cut.tmap", timeout_ms=4000,
                        steps=("type:d", "120,200"))
        need(shown.log, f"(maps) cannot open cut.tmap: {why}")
        if "(maps) shown" in shown.log:
            raise AssertionError("a map was shown from a Pack cut short")


def logs_memory_and_the_tile_tasks_stack() -> None:
    """For the hardware check: internal RAM around the view's making, and the
    tile task's stack, after its first view and as it stops on leaving."""
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=4500, steps=("home",))
    need(shown.log, "(map_view) map view made, its tile task started; internal RAM")
    need(shown.log, "(map_view) tile task after its first view;")
    need(shown.log, "(map_view) tile task stopping;")


CASES: tuple[tuple[str, Callable[[], None]], ...] = (
    ("opens-the-pack-on-the-first-region", opens_the_pack_on_the_first_region),
    ("shows-the-packs-tiles-pixel-for-pixel", shows_the_packs_tiles_pixel_for_pixel),
    ("arabic-is-shaped-and-joined-on-the-screen", arabic_is_shaped_and_joined_on_the_screen),
    ("the-credit-is-on-the-about-line", the_credit_is_on_the_about_line),
    ("no-pack-says-where-to-put-one", no_pack_says_where_to_put_one),
    ("a-file-that-is-no-pack-is-refused", a_file_that_is_no_pack_is_refused),
    ("a-newer-format-is-refused-with-a-message", a_newer_format_is_refused_with_a_message),
    ("pans-half-a-screen-a-step-with-one-refresh-each",
     pans_half_a_screen_a_step_with_one_refresh_each),
    ("zooms-through-the-four-levels", zooms_through_the_four_levels),
    ("outside-every-region-is-hatched", outside_every_region_is_hatched),
    ("a-bit-flipped-tile-is-hatched-and-the-rest-shown",
     a_bit_flipped_tile_is_hatched_and_the_rest_shown),
    ("a-truncated-pack-is-refused-and-the-app-goes-on",
     a_truncated_pack_is_refused_and_the_app_goes_on),
    ("logs-memory-and-the-tile-tasks-stack", logs_memory_and_the_tile_tasks_stack),
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
