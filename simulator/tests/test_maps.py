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
            steps: tuple[str, ...] = (), extra: tuple[str, ...] = (),
            fix: Optional[list[tuple[int, float, float]]] = None,
            files: Optional[dict[str, str]] = None) -> Shown:
    """Runs the app on a scratch card holding pack. Each step is a --tap
    (X,Y, or type:KEYS) or a --drag (X1,Y1,X2,Y2), 1.5 s apart, the first
    2.25 s after startup. fix is the GPS fix's path, (ms, lat, lon) each
    from ms after startup; without it, the simulator's fix, far from every
    Region. files are more files for the card, by path."""
    with tempfile.TemporaryDirectory(prefix="thistle-maps-") as tmp:
        card = Path(tmp) / "card"
        shutil.copytree(SDCARD, card)
        maps = card / "maps"
        shutil.rmtree(maps, ignore_errors=True)
        maps.mkdir()
        if pack is not None:
            (maps / name).write_bytes(pack)
        for path, text in (files or {}).items():
            (card / path).parent.mkdir(parents=True, exist_ok=True)
            (card / path).write_text(text)
        shot_path = Path(tmp) / "shot.ppm"
        env = dict(__import__("os").environ, THISTLE_SIM_SDCARD=str(card))
        if fix:
            env["THISTLE_SIM_GPS_PATH"] = ";".join(f"{ms}:{lat:.9f},{lon:.9f}"
                                                   for ms, lat, lon in fix)
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


def controls(log: str) -> list[tuple[int, int, int, int]]:
    """The rectangles of the app's buttons over the map, in the view's
    pixels, as the app logs them: "(maps) locate button X1,Y1-X2,Y2"."""
    vx, vy, _, _ = view_rect(log)
    found = re.findall(r"\(maps\) \w+ button (\d+),(\d+)-(\d+),(\d+)", log)
    return [(int(a) - vx - 1, int(b) - vy - 1, int(c) - vx + 1, int(d) - vy + 1)
            for a, b, c, d in found]


def check_screen(shown: Shown, pack: Pack, z: int, left: int, top: int,
                 damaged: frozenset = frozenset(), skip: tuple = (),
                 ink: Optional[Callable[[int, int], bool]] = None) -> int:
    """Every pixel of the view, but the About line, the buttons and the
    rectangles in skip (the view's pixels, inclusive), against the Pack's
    tiles and the hatching: a tile the Pack lacks, or one in damaged, is
    hatched. ink(gx, gy), if given, says where something drawn over the map
    should show instead, ink or paper (None: the map; "either": not
    checked). Returns how many of the Pack's tiles show."""
    vx, vy, w, h = view_rect(shown.log)
    if (vx, w) != (0, 240):
        raise AssertionError(f"the view is {w} wide at x {vx}, not the screen's width")
    buttons = zoom_buttons(shown.log)
    bx = min(x for x, _ in buttons.values()) - BUTTON_HALF
    by = min(y for _, y in buttons.values()) - BUTTON_HALF - vy
    rects = controls(shown.log) + list(skip)
    index = pack.index()
    tiles: dict = {}
    wrong = []
    for sy in range(h - ABOUT_H):
        for sx in range(w):
            if sx >= bx and sy >= by:
                continue
            if any(x1 <= sx <= x2 and y1 <= sy <= y2 for x1, y1, x2, y2 in rects):
                continue
            gx, gy = left + sx, top + sy
            key = (z, gx // 256, gy // 256)
            over = ink(gx, gy) if ink else None
            if over == "either":
                continue
            if over is not None:
                want = over
            elif key in index and key not in damaged:
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

ACTION = re.compile(r"\(map_view\) (pan \w+|zoom in|zoom out|already at z\d+|locate|page jump|"
                    r"overlay \S+ o(?:n|ff)\b)")
# A step that draws over the view on the screen, as switching an overlay
# does, with no view swapped in.
IN_PLACE = "in place"
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


def expect_steps(log: str, want: list[tuple[str, object]]) -> None:
    """Each step as wanted: its action, and the view it swapped in, with one
    refresh, drawn whole; for IN_PLACE, no view swapped in but one refresh;
    or, for None, nothing drawn and no refresh."""
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
        if view == IN_PLACE:
            if step.shown:
                raise AssertionError(f"{name} swapped in {step.shown}, not drawn in place")
            if step.refreshes != 1:
                raise AssertionError(f"{name} cost {step.refreshes} refreshes, not one")
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


# --------------------------------------------------------------------------
# The position: opening on it, locate, the marker and the page jump
# --------------------------------------------------------------------------

MARKER_HALF = 11      # the marker is 21 px a side, centred on the fix
MOVE_PX = 12          # the marker is redrawn only past this
MARKER_GAP_S = 5.0    # and at most this often
EDGE_PX = 24          # nearer the view's edge than this, the map jumps a step

MARKER = re.compile(r"\(map_view\) marker (?:at|moved \d+ px to) (\d+),(\d+)")
MARKER_OFF = "(map_view) marker off the view"


def lat_lon(gx: float, gy: float, z: int) -> tuple[float, float]:
    """The inverse of world_pixel: where world pixel (gx, gy) is."""
    size = 256 * (1 << z)
    lon = gx / size * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * gy / size))))
    return lat, lon


def medina(pack: Pack) -> tuple[int, int]:
    """The first Region's centre, the world pixel at z16."""
    lat, lon = pack.regions()[0]["centre"]
    gx, gy = world_pixel(lat, lon, 16)
    return math.floor(gx + 0.5), math.floor(gy + 0.5)


def fix_at(ms: int, gx: int, gy: int) -> tuple[int, float, float]:
    """A fix from ms after startup at world pixel (gx, gy) of z16."""
    return (ms, *lat_lon(gx, gy, 16))


def marker_skip(x: int, y: int) -> tuple[int, int, int, int]:
    return (x - MARKER_HALF, y - MARKER_HALF, x + MARKER_HALF, y + MARKER_HALF)


def markers(log: str) -> list[tuple[int, int]]:
    return [(int(x), int(y)) for x, y in MARKER.findall(log)]


def check_marker(shown: Shown, x: int, y: int) -> None:
    """The marker on the screen at (x, y) of the view: a black dot in a ring,
    paper between them."""
    vx, vy, _, _ = view_rect(shown.log)
    sx, sy = vx + x, vy + y
    if not dark(shown.shot, sx, sy):
        raise AssertionError(f"no marker dot at {x},{y} of the view")
    ring = [dark(shown.shot, sx + dx, sy) for dx in range(-MARKER_HALF + 1, MARKER_HALF)]
    if ring.count(True) < 5 or ring.count(False) < 4:
        raise AssertionError(f"no marker at {x},{y}: across it {ring}")


def opens_on_the_fix_with_the_marker_in_the_middle() -> None:
    """With a fix inside a Region, the map opens around it, the marker in the
    middle of the view and the map around it as the Pack has it."""
    pack = Pack(GOLDEN.read_bytes())
    cx, cy = medina(pack)
    gx, gy = cx + 37, cy - 21
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=3500, fix=[fix_at(0, gx, gy)])
    lat, lon = lat_lon(gx, gy, 16)
    need(shown.log, f"(maps) showing z16 around {lat:.7f}, {lon:.7f} (your position)")
    vx, vy, w, h = view_rect(shown.log)
    left, top = gx - w // 2, gy - h // 2
    if first_shown(shown.log) != (16, left, top):
        raise AssertionError(f"the map opened at {first_shown(shown.log)}, not z16 {left},{top}")
    if markers(shown.log) != [(w // 2, h // 2)]:
        raise AssertionError(f"the marker was drawn at {markers(shown.log)}")
    check_marker(shown, w // 2, h // 2)
    check_screen(shown, pack, 16, left, top, skip=(marker_skip(w // 2, h // 2),))


def opens_on_the_region_without_a_fix_there() -> None:
    """A fix outside every Region (the simulator's own, in San Francisco):
    the map opens on the first Region, and no marker shows."""
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=3500)
    need(shown.log, "(maps) showing z16 around 36.8000000, 10.1800000 (Medina)")
    if markers(shown.log):
        raise AssertionError(f"a marker was drawn at {markers(shown.log)}")


def control_centre(log: str, name: str) -> str:
    found = re.search(rf"\(maps\) {name} button (\d+),(\d+)-(\d+),(\d+)", log)
    if not found:
        raise AssertionError(f"the app did not say where its {name} button is")
    x1, y1, x2, y2 = (int(v) for v in found.groups())
    return f"{(x1 + x2) // 2},{(y1 + y2) // 2}"


def locate_after_panning_away() -> None:
    """Two steps east take the marker off the view; locate, by its button or
    the l key, brings the fix back to the middle, marker and all, in one
    refresh."""
    pack = Pack(GOLDEN.read_bytes())
    cx, cy = medina(pack)
    gx, gy = cx - 15, cy + 9
    probe = run_app(GOLDEN.read_bytes(), timeout_ms=1500)
    for locate in (control_centre(probe.log, "locate"), "type:l"):
        shown = run_app(GOLDEN.read_bytes(), timeout_ms=8000, fix=[fix_at(0, gx, gy)],
                        steps=("type:d", "type:d", locate))
        vx, vy, w, h = view_rect(shown.log)
        left, top = gx - w // 2, gy - h // 2
        expect_steps(shown.log, [("pan east", (16, left + w // 2, top)),
                                 ("pan east", (16, left + w, top)),
                                 ("locate", (16, left, top))])
        after = shown.log.split("(map_view) locate")[1]
        before = shown.log.split("(map_view) locate")[0]
        if MARKER_OFF not in before:
            raise AssertionError("the marker stayed on the view two steps away")
        if markers(after) != [(w // 2, h // 2)]:
            raise AssertionError(f"after locate the marker was drawn at {markers(after)}")
        check_marker(shown, w // 2, h // 2)
        check_screen(shown, pack, 16, left, top, skip=(marker_skip(w // 2, h // 2),))


HELD = re.compile(r"\(map_view\) marker held: (\d+) px from where it is drawn, "
                  r"(\d+\.\d) s after the last")
MOVED = re.compile(r"\(map_view\) marker moved (\d+) px to (\d+),(\d+), "
                   r"(\d+\.\d) s after the last")


def flushes_after(log: str, line: str) -> list[tuple[int, int, int, int]]:
    """The flushes that followed line, up to the refresh that showed them."""
    rest = log.split(line, 1)[1]
    found = []
    for l in rest.splitlines():
        if REFRESH in l:
            break
        if (f := FLUSH.search(l)):
            found.append(tuple(int(v) for v in f.groups()))
    return found


def the_marker_moves_past_12_px_at_most_every_5_s() -> None:
    """A fix that moves 30 px soon after the marker was drawn waits out the
    5 s, then the marker moves, refreshing only around it; 8 px more, and
    then 4 px more (12 in all), and it stays where it is."""
    pack = Pack(GOLDEN.read_bytes())
    cx, cy = medina(pack)
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=13000, fix=[
        fix_at(0, cx, cy), fix_at(2500, cx + 30, cy), fix_at(8500, cx + 38, cy),
        fix_at(10500, cx + 42, cy)])
    vx, vy, w, h = view_rect(shown.log)
    mx, my = w // 2, h // 2
    held = [(int(px), float(s)) for px, s in HELD.findall(shown.log)]
    moved = [(int(px), int(x), int(y), float(s)) for px, x, y, s in MOVED.findall(shown.log)]
    if [m[:3] for m in moved] != [(30, mx + 30, my)]:
        raise AssertionError(f"the marker moved {moved}, not once, 30 px")
    if moved[0][3] < MARKER_GAP_S:
        raise AssertionError(f"the marker moved {moved[0][3]} s after the last, under 5 s")
    if not held or held[0][0] != 30 or held[0][1] >= MARKER_GAP_S:
        raise AssertionError(f"a move of 30 px within 5 s was not held: {held}")
    if [p for p, _ in held[1:]] != [8, 12]:
        raise AssertionError(f"after the move, held {held[1:]}, not 8 px and 12 px")
    if "(map_view) page jump" in shown.log or len(steps_of(shown.log)) != 0:
        raise AssertionError("the map moved")
    # Only the marker's old and new places are drawn again.
    for x1, y1, x2, y2 in flushes_after(shown.log, "(map_view) marker moved"):
        if x2 - x1 > 2 * MARKER_HALF + 30 + 2 or y2 - y1 > 2 * MARKER_HALF + 2:
            raise AssertionError(f"the marker's move flushed ({x1},{y1})-({x2},{y2})")
    left, top = cx - w // 2, cy - h // 2
    check_marker(shown, mx + 30, my)
    check_screen(shown, pack, 16, left, top, skip=(marker_skip(mx + 30, my),))


def the_map_jumps_a_step_when_the_marker_nears_the_edge() -> None:
    """Walking east to within 24 px of the view's edge: the map moves a step
    east, half the view, the marker with it, in one refresh."""
    pack = Pack(GOLDEN.read_bytes())
    cx, cy = medina(pack)
    vx, vy, w, h = view_rect(run_app(GOLDEN.read_bytes(), timeout_ms=1500).log)
    edge = cx - w // 2 + w - EDGE_PX + 2
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=10000, fix=[
        fix_at(0, cx, cy), fix_at(6500, edge, cy)])
    left, top = cx - w // 2, cy - h // 2
    need(shown.log, f"(map_view) page jump east to keep the marker in view: z16 at "
                    f"{left + w // 2},{top}")
    expect_steps(shown.log, [("page jump", (16, left + w // 2, top))])
    after = shown.log.split("(map_view) page jump")[1]
    x = edge - (left + w // 2)
    if markers(after) != [(x, h // 2)]:
        raise AssertionError(f"after the jump the marker was drawn at {markers(after)}")
    check_marker(shown, x, h // 2)
    check_screen(shown, pack, 16, left + w // 2, top, skip=(marker_skip(x, h // 2),))


# --------------------------------------------------------------------------
# The Navigator's recorded tracks
# --------------------------------------------------------------------------

def gpx(points: list[tuple[float, float]]) -> str:
    """A track as the Navigator records it (apps/navigator)."""
    rows = "".join(f'    <trkpt lat="{lat:.7f}" lon="{lon:.7f}"><ele>12.0</ele>'
                   f"<time>2026-11-01T10:15:00Z</time></trkpt>\n" for lat, lon in points)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<gpx version="1.1" creator="ThistleOS Navigator">\n'
            "  <trk><name>ThistleOS Track</name><trkseg>\n" + rows +
            "  </trkseg></trk>\n</gpx>\n")


def segment_distance(px: float, py: float, a: tuple, b: tuple) -> float:
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) /
                                                (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


# A track is drawn as a black line 3 px wide in a paper casing 7 px wide:
# ink within 1.5 px of it, paper to 3.5 px. Pixels near either edge are not
# checked, for rounding.
TRACK_INK, TRACK_CASING = 1.5, 3.5


def track_ink(line: list[tuple[float, float]]):
    def ink(gx: int, gy: int):
        d = min(segment_distance(gx + 0.5, gy + 0.5, a, b) for a, b in zip(line, line[1:]))
        if d < TRACK_INK - 0.2:
            return True
        if d < TRACK_INK + 0.2:
            return "either"
        if d < TRACK_CASING - 0.2:
            return False
        if d < TRACK_CASING + 0.2:
            return "either"
        return None
    return ink


def a_recorded_track_is_drawn_and_can_be_switched_off() -> None:
    """A track the Navigator recorded in /sdcard/tracks/ is drawn over the
    map, and stays drawn as the map moves; the Tracks button takes it off,
    and t puts it back, a refresh each, drawn over the view in place."""
    pack = Pack(GOLDEN.read_bytes())
    cx, cy = medina(pack)
    corners = [(cx - 100, cy - 60), (cx + 20, cy - 60), (cx + 20, cy + 80), (cx + 170, cy + 90)]
    points = [lat_lon(x, y, 16) for x, y in corners]
    files = {"tracks/2026-11-01_101500.gpx": gpx(points)}
    probe = run_app(GOLDEN.read_bytes(), timeout_ms=1500, files=files)
    need(probe.log, "(maps) tracks: 1 track, 4 points")
    vx, vy, w, h = view_rect(probe.log)
    left, top = expected_origin(pack, 16, w, h)
    line = [world_pixel(lat, lon, 16) for lat, lon in points]
    check_screen(probe, pack, 16, left, top, ink=track_ink(line))

    # A step east: still drawn where it is on the map.
    east = run_app(GOLDEN.read_bytes(), timeout_ms=4000, files=files, steps=("type:d",))
    check_screen(east, pack, 16, left + w // 2, top, ink=track_ink(line))

    # Off by the button: the map alone; on again by the key.
    off = run_app(GOLDEN.read_bytes(), timeout_ms=4000, files=files,
                  steps=(control_centre(probe.log, "tracks"),))
    expect_steps(off.log, [('overlay "tracks" off', IN_PLACE)])
    check_screen(off, pack, 16, left, top)
    on = run_app(GOLDEN.read_bytes(), timeout_ms=5500, files=files,
                 steps=(control_centre(probe.log, "tracks"), "type:t"))
    expect_steps(on.log, [('overlay "tracks" off', IN_PLACE),
                          ('overlay "tracks" on', IN_PLACE)])
    check_screen(on, pack, 16, left, top, ink=track_ink(line))


def no_tracks_no_button() -> None:
    shown = run_app(GOLDEN.read_bytes(), timeout_ms=1500)
    need(shown.log, "(maps) tracks: none in /sdcard/tracks")
    if "(maps) tracks button" in shown.log:
        raise AssertionError("a Tracks button with no track")


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
    ("opens-on-the-fix-with-the-marker-in-the-middle",
     opens_on_the_fix_with_the_marker_in_the_middle),
    ("opens-on-the-region-without-a-fix-there", opens_on_the_region_without_a_fix_there),
    ("locate-after-panning-away", locate_after_panning_away),
    ("the-marker-moves-past-12-px-at-most-every-5-s",
     the_marker_moves_past_12_px_at_most_every_5_s),
    ("the-map-jumps-a-step-when-the-marker-nears-the-edge",
     the_map_jumps_a_step_when_the_marker_nears_the_edge),
    ("a-recorded-track-is-drawn-and-can-be-switched-off",
     a_recorded_track_is_drawn_and_can_be_switched_off),
    ("no-tracks-no-button", no_tracks_no_button),
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
